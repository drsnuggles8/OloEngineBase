#include "SceneHierarchyPanel.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Localization/LocalizationManager.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"

#include <random>
#include <glm/gtc/matrix_transform.hpp>
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Audio/AudioEvents/AudioCommandRegistry.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/UI/UI.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Particle/EmissionShapeUtils.h"
#include "OloEngine/Particle/ParticlePresets.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Renderer/LightProbeVolumeAsset.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Scene/Streaming/StreamingRegionSerializer.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Animation/MorphTargets/FacialExpressionLibrary.h"
#include "OloEngine/Gameplay/Inventory/InventoryComponents.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "../UndoRedo/EntityCommands.h"
#include "../UndoRedo/ComponentCommands.h"
#include "../UndoRedo/SpecializedCommands.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>
#include <cctype>
#include <concepts>
#include <unordered_map>
#include <algorithm>

namespace
{
    // Interprets ImGui drag-drop payload bytes as a UTF-8 path.
    [[nodiscard]] std::filesystem::path PathFromUtf8Payload(const ImGuiPayload& payload)
    {
        auto const* data = static_cast<char const*>(payload.Data);
        auto const* u8data = reinterpret_cast<char8_t const*>(data);
        // Strip trailing NUL if the sender included it in DataSize
        size_t len = static_cast<size_t>(payload.DataSize);
        if (len > 0 && data[len - 1] == '\0')
            --len;
        return std::filesystem::path(std::u8string_view(u8data, len));
    }
} // namespace

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
        m_SelectedEntities.clear();
    }

    void SceneHierarchyPanel::OnImGuiRender()
    {
        ImGui::Begin("Scene Hierarchy");

        if (m_Context)
        {
            // Search / filter bar
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##EntityFilter", "Search entities...", m_FilterText, sizeof(m_FilterText));
            ImGui::Separator();

            const bool hasFilter = m_FilterText[0] != '\0';

            auto caseInsensitiveFind = [](const std::string& haystack, const char* needle) -> bool
            {
                if (!needle[0])
                {
                    return true;
                }
                auto it = std::search(
                    haystack.begin(), haystack.end(),
                    needle, needle + std::strlen(needle),
                    [](char a, char b)
                    { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
                return it != haystack.end();
            };

            m_Context->m_Registry.view<entt::entity>().each([this, hasFilter, &caseInsensitiveFind](const auto e)
                                                            {
                Entity entity{ e, *m_Context };

                // When filtering, show all matching entities flat.
                // Otherwise, only render root entities (children drawn recursively).
                if (!hasFilter && entity.GetParentUUID() != UUID(0))
                {
                    return;
                }

                if (hasFilter)
                {
                    auto& tag = entity.GetComponent<TagComponent>().Tag;
                    if (!caseInsensitiveFind(tag, m_FilterText))
                    {
                        return;
                    }
                }
                DrawEntityNode(entity); });

            if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
            {
                ClearSelection();
            }

            // Drag-and-Drop: drop on empty space to unparent
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_REPARENT"))
                {
                    UUID droppedUUID = *static_cast<const UUID*>(payload->Data);
                    auto droppedOpt = m_Context->TryGetEntityWithUUID(droppedUUID);
                    if (droppedOpt && droppedOpt->GetParentUUID() != UUID(0))
                    {
                        UUID oldParent = droppedOpt->GetParentUUID();
                        if (Entity parent = droppedOpt->GetParent(); parent)
                        {
                            parent.RemoveChild(*droppedOpt);
                        }
                        if (m_CommandHistory)
                        {
                            m_CommandHistory->PushAlreadyExecuted(std::make_unique<ReparentEntityCommand>(
                                m_Context, droppedUUID, oldParent, UUID(0)));
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Right-click on blank space (not on an entity item)
            if (ImGui::BeginPopupContextWindow(nullptr, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
            {
                if (ImGui::MenuItem("Create Empty Entity"))
                {
                    if (m_CommandHistory)
                    {
                        m_CommandHistory->Execute(std::make_unique<CreateEntityCommand>(
                            m_Context, "Empty Entity",
                            [this](Entity e)
                            { SetSelectedEntity(e); },
                            [this]()
                            { ClearSelection(); }));
                    }
                    else
                    {
                        SetSelectedEntity(m_Context->CreateEntity("Empty Entity"));
                    }
                }

                if (ImGui::BeginMenu("Create UI"))
                {
                    auto createUICanvasWithUndo = [this]()
                    {
                        if (m_CommandHistory)
                        {
                            m_CommandHistory->Execute(std::make_unique<CreateEntityCommand>(
                                m_Context, "UI Canvas",
                                [this](Entity e)
                                {
                                    e.AddComponent<UICanvasComponent>();
                                    e.AddComponent<UIRectTransformComponent>();
                                    SetSelectedEntity(e);
                                },
                                [this]()
                                { ClearSelection(); }));
                        }
                        else
                        {
                            auto canvas = m_Context->CreateEntity("UI Canvas");
                            canvas.AddComponent<UICanvasComponent>();
                            canvas.AddComponent<UIRectTransformComponent>();
                            SetSelectedEntity(canvas);
                        }
                    };

                    auto createUIWidgetWithUndo = [this](const char* name, auto addComponentFn)
                    {
                        if (m_CommandHistory)
                        {
                            m_CommandHistory->Execute(std::make_unique<CreateEntityCommand>(
                                m_Context, name,
                                [this, addComponentFn](Entity e)
                                {
                                    e.AddComponent<UIRectTransformComponent>();
                                    addComponentFn(e);
                                    Entity canvas = FindOrCreateCanvas();
                                    e.SetParent(canvas);
                                    SetSelectedEntity(e);
                                },
                                [this]()
                                { ClearSelection(); }));
                        }
                        else
                        {
                            auto widget = CreateUIWidget(name);
                            addComponentFn(widget);
                            SetSelectedEntity(widget);
                        }
                    };

                    if (ImGui::MenuItem("UI Canvas"))
                    {
                        createUICanvasWithUndo();
                    }

                    ImGui::Separator();

                    if (ImGui::MenuItem("Panel"))
                    {
                        createUIWidgetWithUndo("UI Panel", [](Entity e)
                                               { e.AddComponent<UIPanelComponent>(); });
                    }

                    if (ImGui::MenuItem("Text"))
                    {
                        createUIWidgetWithUndo("UI Text", [](Entity e)
                                               { e.AddComponent<UITextComponent>(); });
                    }

                    if (ImGui::MenuItem("Image"))
                    {
                        createUIWidgetWithUndo("UI Image", [](Entity e)
                                               { e.AddComponent<UIImageComponent>(); });
                    }

                    if (ImGui::MenuItem("Button"))
                    {
                        createUIWidgetWithUndo("UI Button", [](Entity e)
                                               { e.AddComponent<UIButtonComponent>(); });
                    }

                    if (ImGui::MenuItem("Slider"))
                    {
                        createUIWidgetWithUndo("UI Slider", [](Entity e)
                                               { e.AddComponent<UISliderComponent>(); });
                    }

                    if (ImGui::MenuItem("Checkbox"))
                    {
                        createUIWidgetWithUndo("UI Checkbox", [](Entity e)
                                               { e.AddComponent<UICheckboxComponent>(); });
                    }

                    if (ImGui::MenuItem("Toggle"))
                    {
                        createUIWidgetWithUndo("UI Toggle", [](Entity e)
                                               { e.AddComponent<UIToggleComponent>(); });
                    }

                    if (ImGui::MenuItem("Progress Bar"))
                    {
                        createUIWidgetWithUndo("UI Progress Bar", [](Entity e)
                                               { e.AddComponent<UIProgressBarComponent>(); });
                    }

                    if (ImGui::MenuItem("Input Field"))
                    {
                        createUIWidgetWithUndo("UI Input Field", [](Entity e)
                                               { e.AddComponent<UIInputFieldComponent>(); });
                    }

                    if (ImGui::MenuItem("Dropdown"))
                    {
                        createUIWidgetWithUndo("UI Dropdown", [](Entity e)
                                               { e.AddComponent<UIDropdownComponent>(); });
                    }

                    if (ImGui::MenuItem("Scroll View"))
                    {
                        createUIWidgetWithUndo("UI Scroll View", [](Entity e)
                                               { e.AddComponent<UIScrollViewComponent>(); });
                    }

                    if (ImGui::MenuItem("Grid Layout"))
                    {
                        createUIWidgetWithUndo("UI Grid Layout", [](Entity e)
                                               { e.AddComponent<UIGridLayoutComponent>(); });
                    }

                    ImGui::EndMenu();
                }

                ImGui::EndPopup();
            }
        }

        ImGui::End();

        ImGui::Begin("Properties");
        if (m_SelectionContext && m_SelectedEntities.size() <= 1)
        {
            DrawComponents(m_SelectionContext);
        }
        else if (m_SelectedEntities.size() > 1)
        {
            ImGui::TextDisabled("%zu entities selected", m_SelectedEntities.size());
        }

        ImGui::End();
    }

    void SceneHierarchyPanel::SetSelectedEntity(const Entity entity)
    {
        m_SelectionContext = entity;
        m_SelectedEntities.clear();
        if (entity)
        {
            m_SelectedEntities.push_back(entity);
        }
    }

    void SceneHierarchyPanel::ClearSelection()
    {
        m_SelectionContext = {};
        m_SelectedEntities.clear();
    }

    void SceneHierarchyPanel::ToggleEntitySelection(Entity entity)
    {
        if (!entity)
        {
            return;
        }

        if (auto it = std::ranges::find(m_SelectedEntities, entity); it != m_SelectedEntities.end())
        {
            m_SelectedEntities.erase(it);
            m_SelectionContext = m_SelectedEntities.empty() ? Entity{} : m_SelectedEntities.back();
        }
        else
        {
            m_SelectedEntities.push_back(entity);
            m_SelectionContext = entity;
        }
    }

    void SceneHierarchyPanel::DeleteSelectedEntities()
    {
        if (m_SelectedEntities.empty())
        {
            return;
        }

        if (m_CommandHistory && m_SelectedEntities.size() > 1)
        {
            auto compound = std::make_unique<CompoundCommand>("Delete " + std::to_string(m_SelectedEntities.size()) + " Entities");
            for (auto& entity : m_SelectedEntities)
            {
                compound->Add(std::make_unique<DeleteEntityCommand>(
                    m_Context, entity,
                    [this]()
                    { ClearSelection(); },
                    [this](Entity restored)
                    { SetSelectedEntity(restored); }));
            }
            m_CommandHistory->Execute(std::move(compound));
            ClearSelection();
        }
        else if (m_SelectedEntities.size() == 1)
        {
            Entity entity = m_SelectedEntities[0];
            if (m_CommandHistory)
            {
                m_CommandHistory->Execute(std::make_unique<DeleteEntityCommand>(
                    m_Context, entity,
                    [this]()
                    { ClearSelection(); },
                    [this](Entity restored)
                    { SetSelectedEntity(restored); }));
            }
            else
            {
                m_Context->DestroyEntity(entity);
                ClearSelection();
            }
        }
    }

    bool SceneHierarchyPanel::IsEntitySelected(Entity entity) const
    {
        return std::ranges::find(m_SelectedEntities, entity) != m_SelectedEntities.end();
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

    void SceneHierarchyPanel::CollectVisualOrder(Entity entity, std::vector<Entity>& out) const
    {
        out.push_back(entity);
        for (const UUID& childUUID : entity.Children())
        {
            auto childOpt = m_Context->TryGetEntityWithUUID(childUUID);
            if (childOpt)
            {
                CollectVisualOrder(*childOpt, out);
            }
        }
    }

    // Helper: revert a list of component overrides on a prefab instance
    static void RevertComponentList(const Ref<Prefab>& prefab, Entity entity,
                                    const std::unordered_set<std::string>& componentNames)
    {
        auto copy = componentNames;
        for (const auto& compName : copy)
            prefab->RevertComponent(entity, compName);
    }

    // Helper: apply a list of component overrides from instance to prefab
    static void ApplyComponentList(Ref<Prefab>& prefab, Entity entity,
                                   const std::unordered_set<std::string>& componentNames)
    {
        auto copy = componentNames;
        for (const auto& compName : copy)
            prefab->ApplyComponentToPrefab(entity, compName);
    }

    void SceneHierarchyPanel::DrawEntityNode(Entity entity)
    {
        auto& tagComponent = entity.GetComponent<TagComponent>();
        auto& tag = tagComponent.Tag;

        ImGuiTreeNodeFlags flags = (IsEntitySelected(entity) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow;
        flags |= ImGuiTreeNodeFlags_SpanAvailWidth;

        // Mark as leaf if entity has no children
        const auto& children = entity.Children();
        if (children.empty())
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        // Visual indicator for prefab instances
        bool isPrefabInstance = entity.HasComponent<PrefabComponent>() && entity.GetComponent<PrefabComponent>().IsValid();
        if (isPrefabInstance)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        }

        bool opened = ImGui::TreeNodeEx((void*)static_cast<u64>(static_cast<u32>(entity)), flags, tag.c_str());

        if (isPrefabInstance)
        {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemClicked())
        {
            const bool ctrl = ImGui::GetIO().KeyCtrl;
            const bool shift = ImGui::GetIO().KeyShift;

            if (ctrl)
            {
                ToggleEntitySelection(entity);
            }
            else if (shift && m_SelectionContext)
            {
                // Shift+click: select range between last clicked and this entity
                // Collect visible entities in visual (tree) order
                std::vector<Entity> visible;
                m_Context->m_Registry.view<entt::entity>().each([this, &visible](const auto e)
                                                                {
                    Entity ent{ e, *m_Context };
                    if (ent.GetParentUUID() == UUID(0))
                    {
                        CollectVisualOrder(ent, visible);
                    } });

                auto itA = std::ranges::find(visible, m_SelectionContext);
                auto itB = std::ranges::find(visible, entity);
                if (itA != visible.end() && itB != visible.end())
                {
                    if (itA > itB)
                    {
                        std::swap(itA, itB);
                    }
                    m_SelectedEntities.clear();
                    for (auto it = itA; it <= itB; ++it)
                    {
                        m_SelectedEntities.push_back(*it);
                    }
                    m_SelectionContext = entity;
                }
            }
            else
            {
                // Regular click: single select
                SetSelectedEntity(entity);
            }
        }

        bool entityDeleted = false;
        bool deleteAll = false;
        ImGui::PushID(static_cast<int>(static_cast<u32>(entity)));
        if (ImGui::BeginPopupContextItem("##EntityCtx"))
        {
            if (ImGui::MenuItem("Rename"))
            {
                tagComponent.renaming = true;
                m_RenameOldName = tag;
            }

            if (ImGui::MenuItem("Delete Entity"))
            {
                entityDeleted = true;
            }

            if (m_SelectedEntities.size() > 1 && IsEntitySelected(entity))
            {
                if (ImGui::MenuItem("Delete All Selected"))
                {
                    deleteAll = true;
                }
            }

            if (entity.GetParentUUID() != UUID(0))
            {
                if (ImGui::MenuItem("Unparent"))
                {
                    UUID oldParentUUID = entity.GetParentUUID();
                    if (Entity parent = entity.GetParent(); parent)
                    {
                        parent.RemoveChild(entity);
                    }
                    if (m_CommandHistory)
                    {
                        m_CommandHistory->PushAlreadyExecuted(std::make_unique<ReparentEntityCommand>(
                            m_Context, entity.GetUUID(), oldParentUUID, UUID(0)));
                    }
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Save as Prefab"))
            {
                auto& entityTag = entity.GetComponent<TagComponent>().Tag;
                std::filesystem::path prefabDir = Project::GetAssetDirectory() / "prefabs";
                std::filesystem::create_directories(prefabDir);
                std::filesystem::path prefabPath = prefabDir / (entityTag + ".oloprefab");

                // Create and populate the prefab
                Ref<Prefab> prefab = Ref<Prefab>::Create();
                AssetHandle handle = AssetManager::AddMemoryOnlyAsset(prefab);
                prefab->Create(entity, false);

                // Register with asset system and serialize to disk
                auto* editorManager = static_cast<EditorAssetManager*>(
                    Project::GetActive()->GetAssetManager().get());
                auto relativePath = Project::GetAssetRelativeFileSystemPath(prefabPath);

                AssetMetadata metadata;
                metadata.Handle = handle;
                metadata.FilePath = relativePath;
                metadata.Type = AssetType::Prefab;
                metadata.IsDataLoaded = true;
                editorManager->SetMetadata(handle, metadata);
                editorManager->SerializeAssetRegistry();

                AssetImporter::Serialize(metadata, prefab);

                // Mark the source entity as a prefab instance
                if (!entity.HasComponent<PrefabComponent>())
                {
                    if (m_CommandHistory)
                    {
                        auto compound = std::make_unique<CompoundCommand>("Save as Prefab");
                        compound->Add(std::make_unique<AddComponentCommand<PrefabComponent>>(m_Context, entity.GetUUID()));
                        PrefabComponent defaultComp;
                        PrefabComponent initComp(handle, entity.GetUUID());
                        compound->Add(std::make_unique<ComponentChangeCommand<PrefabComponent>>(
                            m_Context, entity.GetUUID(), defaultComp, initComp, "Init Prefab"));
                        m_CommandHistory->Execute(std::move(compound));
                    }
                    else
                    {
                        entity.AddComponent<PrefabComponent>(handle, entity.GetUUID());
                    }
                }

                OLO_CORE_INFO("Saved prefab: {}", prefabPath.string());
            }

            // Prefab instance management
            if (isPrefabInstance)
            {
                ImGui::Separator();

                if (ImGui::MenuItem("Update from Prefab"))
                {
                    Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(
                        entity.GetComponent<PrefabComponent>().m_PrefabID);
                    if (prefab)
                    {
                        prefab->UpdateInstanceFromPrefab(entity);
                        OLO_CORE_INFO("Updated instance '{}' from prefab", tag);
                    }
                }

                if (ImGui::MenuItem("Revert All Overrides"))
                {
                    auto& pc = entity.GetComponent<PrefabComponent>();
                    Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(pc.m_PrefabID);
                    if (prefab)
                    {
                        RevertComponentList(prefab, entity, pc.m_OverriddenComponents);
                        RevertComponentList(prefab, entity, pc.m_AddedComponents);
                        RevertComponentList(prefab, entity, pc.m_RemovedComponents);
                        pc.ClearAllOverrides();
                        OLO_CORE_INFO("Reverted all overrides on '{}'", tag);
                    }
                }

                if (entity.GetComponent<PrefabComponent>().HasAnyOverrides())
                {
                    if (ImGui::MenuItem("Apply All Overrides to Prefab"))
                    {
                        auto& pc = entity.GetComponent<PrefabComponent>();
                        Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(pc.m_PrefabID);
                        if (prefab)
                        {
                            ApplyComponentList(prefab, entity, pc.m_OverriddenComponents);
                            ApplyComponentList(prefab, entity, pc.m_AddedComponents);
                            ApplyComponentList(prefab, entity, pc.m_RemovedComponents);
                            pc.ClearAllOverrides();
                            OLO_CORE_INFO("Applied all overrides from '{}' to prefab", tag);
                        }
                    }
                }
            }

            ImGui::EndPopup();
        }
        ImGui::PopID();

        // Drag-and-Drop: source
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            UUID entityUUID = entity.GetUUID();
            ImGui::SetDragDropPayload("ENTITY_REPARENT", &entityUUID, sizeof(UUID));
            ImGui::Text("%s", tag.c_str());
            ImGui::EndDragDropSource();
        }

        // Drag-and-Drop: target
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_REPARENT"))
            {
                UUID droppedUUID = *static_cast<const UUID*>(payload->Data);
                auto droppedOpt = m_Context->TryGetEntityWithUUID(droppedUUID);
                if (droppedOpt && *droppedOpt != entity && !droppedOpt->WouldCreateCycleWith(entity))
                {
                    UUID oldParent = droppedOpt->GetParentUUID();
                    droppedOpt->SetParent(entity);
                    if (m_CommandHistory)
                    {
                        m_CommandHistory->PushAlreadyExecuted(std::make_unique<ReparentEntityCommand>(
                            m_Context, droppedUUID, oldParent, entity.GetUUID()));
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (tagComponent.renaming)
        {
            char buffer[256];
            ::memset(buffer, 0, sizeof(buffer));
            std::strncpy(buffer, tag.c_str(), sizeof(buffer) - 1);
            if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
            {
                std::string oldTag = tag;
                tag = std::string(buffer);
                m_Context->UpdateEntityName(entity, oldTag, tag);
            }

            if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered())
            {
                tagComponent.renaming = false;
                if (m_CommandHistory && tag != m_RenameOldName)
                {
                    m_CommandHistory->PushAlreadyExecuted(std::make_unique<RenameEntityCommand>(
                        m_Context, entity.GetUUID(), m_RenameOldName, tag));
                }
            }
        }

        if (opened && !children.empty())
        {
            for (const UUID& childUUID : children)
            {
                auto childOpt = m_Context->TryGetEntityWithUUID(childUUID);
                if (childOpt)
                {
                    DrawEntityNode(*childOpt);
                }
            }
            ImGui::TreePop();
        }

        if (deleteAll)
        {
            DeleteSelectedEntities();
        }
        else if (entityDeleted)
        {
            if (m_CommandHistory)
            {
                m_CommandHistory->Execute(std::make_unique<DeleteEntityCommand>(
                    m_Context, entity,
                    [this]()
                    { ClearSelection(); },
                    [this](Entity restored)
                    { SetSelectedEntity(restored); }));
            }
            else
            {
                m_Context->DestroyEntity(entity);
            }

            if (m_SelectionContext == entity)
            {
                ClearSelection();
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
                                        f32 valueMin = 0.0f, f32 valueMax = 1.0f,
                                        ImU32 lineColor = IM_COL32(220, 220, 80, 255))
    {
        OLO_PROFILE_FUNCTION();

        bool modified = false;
        ImGui::PushID(label);

        const f32 canvasWidth = ImGui::GetContentRegionAvail().x;
        constexpr f32 canvasHeight = 100.0f;
        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize(canvasWidth, canvasHeight);

        ImGui::InvisibleButton("##curve_canvas", canvasSize);
        const bool isHovered = ImGui::IsItemHovered();
        const bool isActive = ImGui::IsItemActive();

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
        auto toScreen = [&canvasPos, &canvasSize, &valueMin, &valueRange](f32 time, f32 value) -> ImVec2
        {
            f32 ny = (valueRange > 0.0f) ? (value - valueMin) / valueRange : 0.5f;
            return { canvasPos.x + time * canvasSize.x,
                     canvasPos.y + (1.0f - ny) * canvasSize.y };
        };
        auto fromScreen = [&canvasPos, &canvasSize, &valueMin, &valueMax, &valueRange](ImVec2 screen) -> std::pair<f32, f32>
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
                drawList->AddLine(prev, cur, lineColor, 1.5f);
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
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hoveredKey >= 0 && curve.KeyCount > 2)
        {
            auto rk = static_cast<u32>(hoveredKey);
            for (u32 j = rk; j < curve.KeyCount - 1; ++j)
                curve.Keys[j] = curve.Keys[j + 1];
            curve.KeyCount--;
            modified = true;
        }

        // Double-click on empty area: add key (max 8)
        if (isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoveredKey < 0 && curve.KeyCount < 8)
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
        OLO_PROFILE_FUNCTION();

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
        OLO_PROFILE_FUNCTION();

        bool modified = false;
        ImGui::PushID(label);

        // Gradient preview
        DrawGradientBar(curve, ImGui::GetContentRegionAvail().x, 20.0f);

        // Per-channel curve editors in tree nodes
        struct ChannelInfo
        {
            const char* name;
            ParticleCurve* ch;
            ImU32 lineColor;
        };
        ChannelInfo channels[] = {
            { "Red", &curve.R, IM_COL32(255, 80, 80, 255) },
            { "Green", &curve.G, IM_COL32(80, 255, 80, 255) },
            { "Blue", &curve.B, IM_COL32(80, 130, 255, 255) },
            { "Alpha", &curve.A, IM_COL32(200, 200, 200, 255) },
        };
        for (auto& [name, ch, color] : channels)
        {
            if (ImGui::TreeNode(name))
            {
                modified |= DrawParticleCurveEditor(name, *ch, 0.0f, 1.0f, color);
                ImGui::TreePop();
            }
        }

        ImGui::PopID();
        return modified;
    }

    // File-scope state for undo integration in DrawComponent (set by DrawComponents)
    static CommandHistory* s_DrawComponentCmdHistory = nullptr;
    static Ref<Scene> s_DrawComponentScene = nullptr;

    // Compile-time stable component name extraction (no RTTI dependency).
    // Uses a probe type in the OloEngine namespace to calibrate __FUNCSIG__/__PRETTY_FUNCTION__.
    struct ComponentNameProbe_;

    namespace detail
    {
        template<typename T>
        constexpr std::string_view RawComponentSig()
        {
#if defined(_MSC_VER)
            return { __FUNCSIG__, sizeof(__FUNCSIG__) - 1 };
#elif defined(__clang__) || defined(__GNUC__)
            return { __PRETTY_FUNCTION__, sizeof(__PRETTY_FUNCTION__) - 1 };
#endif
        }

        inline constexpr auto kProbeSig = RawComponentSig<ComponentNameProbe_>();
        inline constexpr auto kProbeTarget = std::string_view{ "ComponentNameProbe_" };
        inline constexpr auto kProbePos = kProbeSig.find(kProbeTarget);
        static_assert(kProbePos != std::string_view::npos, "Cannot find probe type in __FUNCSIG__");
        inline constexpr std::size_t kPrefixLen = kProbePos;
        inline constexpr std::size_t kSuffixLen = kProbeSig.size() - kProbePos - kProbeTarget.size();
    } // namespace detail

    template<typename T>
    static std::string GetCanonicalComponentName()
    {
        constexpr auto sig = detail::RawComponentSig<T>();
        constexpr auto fullName = sig.substr(detail::kPrefixLen, sig.size() - detail::kPrefixLen - detail::kSuffixLen);
        // fullName may be "OloEngine::TransformComponent" — extract unqualified name
        constexpr auto lastColon = fullName.rfind(':');
        if constexpr (lastColon != std::string_view::npos)
            return std::string(fullName.substr(lastColon + 1));
        else
            return std::string(fullName);
    }

    // Trait to opt specific trivially-copyable types into value-comparison (operator==)
    // instead of the default byte-level memcmp path.  Specialize to std::true_type for
    // components whose operator== handles semantic equality (e.g., UUID comparison via
    // static_cast) more accurately than raw byte comparison.
    template<typename T>
    struct PreferValueComparison : std::false_type
    {
    };

    template<>
    struct PreferValueComparison<IKTargetComponent> : std::true_type
    {
    };

    template<typename T, typename UIFunction>
    static void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
    {
        // Canonical key for PrefabComponent lookups (e.g. "TransformComponent")
        const std::string componentKey = GetCanonicalComponentName<T>();

        const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_FramePadding;
        if (entity.HasComponent<T>())
        {
            ImGui::PushID(componentKey.c_str());

            auto& component = entity.GetComponent<T>();
            const ImVec2 contentRegionAvailable = ImGui::GetContentRegionAvail();

            // Check if this component is overridden on a prefab instance
            bool isPrefabOverride = false;
            bool isPrefabAdded = false;
            if (entity.HasComponent<PrefabComponent>())
            {
                const auto& pc = entity.GetComponent<PrefabComponent>();
                if (pc.IsValid())
                {
                    isPrefabOverride = pc.IsComponentOverridden(componentKey);
                    isPrefabAdded = pc.IsComponentAdded(componentKey);
                }
            }

            // Override visual: bold orange bar on the left for overridden components
            if (isPrefabOverride)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.6f, 0.4f, 0.1f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.7f, 0.5f, 0.15f, 0.8f));
            }
            else if (isPrefabAdded)
            {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.1f, 0.5f, 0.1f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.15f, 0.6f, 0.15f, 0.8f));
            }

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4, 4 });
            const f32 lineHeight = ImGui::GetFontSize() + (::GImGui->Style.FramePadding.y * 2.0f);
            ImGui::Separator();

            // Build display name with override indicator
            std::string displayName = name;
            if (isPrefabOverride)
                displayName = "* " + name + " (Override)";
            else if (isPrefabAdded)
                displayName = "+ " + name + " (Added)";

            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(typeid(T).hash_code()), treeNodeFlags, displayName.c_str());
            ImGui::PopStyleVar();

            if (isPrefabOverride || isPrefabAdded)
            {
                ImGui::PopStyleColor(2);
            }

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

                // Prefab override context menu items
                if (entity.HasComponent<PrefabComponent>())
                {
                    const auto& pc = entity.GetComponent<PrefabComponent>();
                    if (pc.IsValid())
                    {
                        ImGui::Separator();

                        if (isPrefabOverride || isPrefabAdded)
                        {
                            if (ImGui::MenuItem("Revert to Prefab"))
                            {
                                if (s_DrawComponentScene)
                                {
                                    s_DrawComponentScene->RevertPrefabComponent(entity, componentKey);
                                }
                            }
                            if (ImGui::MenuItem("Apply to Prefab"))
                            {
                                if (s_DrawComponentScene)
                                {
                                    s_DrawComponentScene->ApplyPrefabComponent(entity, componentKey);
                                }
                            }
                        }
                        else
                        {
                            if (ImGui::MenuItem("Mark as Override"))
                            {
                                if (s_DrawComponentScene)
                                {
                                    s_DrawComponentScene->MarkPrefabComponentOverridden(entity, componentKey);
                                }
                            }
                        }
                    }
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();

            if (open)
            {
                // ── Component-level undo tracking ──
                if (s_DrawComponentCmdHistory && s_DrawComponentScene)
                {
                    struct EditState
                    {
                        bool isEditing = false;
                        bool snapshotValid = false;
                        T snapshot{};
                        // Byte-level copy of the snapshot, populated via memcpy so that
                        // padding bytes match those of the live component.  Avoids false
                        // positives when comparing snapshot vs current via memcmp.
                        alignas(alignof(T)) unsigned char snapshotBytes[sizeof(T)]{};
                    };
                    static std::unordered_map<u64, EditState> s_EditStates;
                    auto& editState = s_EditStates[static_cast<u64>(entity.GetUUID()) ^ typeid(T).hash_code()];

                    // Take a snapshot once per idle→edit cycle (not every frame)
                    if (!editState.isEditing && !editState.snapshotValid)
                    {
                        editState.snapshot = component;
                        if constexpr (std::is_trivially_copyable_v<T>)
                        {
                            std::memcpy(editState.snapshotBytes, &component, sizeof(T));
                        }
                        editState.snapshotValid = true;
                    }

                    // Prefab-aware undo push — shared by both tracking strategies
                    auto pushUndoCommand = [&entity, &componentKey, &editState, &component]()
                    {
                        if (entity.HasComponent<PrefabComponent>())
                        {
                            auto& pc = entity.GetComponent<PrefabComponent>();
                            if (pc.IsValid() && !pc.IsComponentOverridden(componentKey))
                            {
                                PrefabComponent pcBefore = pc;
                                pc.MarkComponentOverridden(componentKey);

                                auto compound = std::make_unique<CompoundCommand>("Property Change");
                                compound->Add(std::make_unique<ComponentChangeCommand<T>>(
                                    s_DrawComponentScene, entity.GetUUID(),
                                    editState.snapshot, component, "Property Change"));
                                compound->Add(std::make_unique<ComponentChangeCommand<PrefabComponent>>(
                                    s_DrawComponentScene, entity.GetUUID(),
                                    pcBefore, pc, "Mark Override"));
                                s_DrawComponentCmdHistory->PushAlreadyExecuted(std::move(compound));
                            }
                            else
                            {
                                s_DrawComponentCmdHistory->PushAlreadyExecuted(
                                    std::make_unique<ComponentChangeCommand<T>>(
                                        s_DrawComponentScene, entity.GetUUID(),
                                        editState.snapshot, component, "Property Change"));
                            }
                        }
                        else
                        {
                            s_DrawComponentCmdHistory->PushAlreadyExecuted(
                                std::make_unique<ComponentChangeCommand<T>>(
                                    s_DrawComponentScene, entity.GetUUID(),
                                    editState.snapshot, component, "Property Change"));
                        }
                    };

                    if constexpr (std::is_trivially_copyable_v<T> && !PreferValueComparison<T>::value)
                    {
                        // Byte-level change detection: compare component bytes before and after uiFunction
                        alignas(alignof(T)) unsigned char bytesBefore[sizeof(T)];
                        std::memcpy(bytesBefore, &component, sizeof(T));

                        uiFunction(component);

                        const bool componentChanged = (std::memcmp(bytesBefore, &component, sizeof(T)) != 0);

                        if (componentChanged && !editState.isEditing)
                        {
                            editState.isEditing = true;
                        }

                        // When no change this frame and no active ImGui widget → editing has ended
                        if (editState.isEditing && !componentChanged && ::GImGui->ActiveId == 0)
                        {
                            // Only push if the component actually differs from the original snapshot
                            if (std::memcmp(editState.snapshotBytes, &component, sizeof(T)) != 0)
                            {
                                pushUndoCommand();
                            }
                            editState.isEditing = false;
                            editState.snapshotValid = false;
                        }
                    }
                    else if constexpr (std::equality_comparable<T>)
                    {
                        // Value-level change detection for non-trivially-copyable types with operator==
                        // Compare against snapshot instead of per-frame copy to avoid expensive copies each frame
                        uiFunction(component);

                        const bool diffFromSnapshot = !(editState.snapshot == component);

                        if (diffFromSnapshot && !editState.isEditing)
                        {
                            editState.isEditing = true;
                        }

                        if (editState.isEditing && ::GImGui->ActiveId == 0)
                        {
                            if (diffFromSnapshot)
                            {
                                pushUndoCommand();
                            }
                            editState.isEditing = false;
                            editState.snapshotValid = false;
                        }
                    }
                    else
                    {
                        // Types without comparison support: no undo tracking
                        uiFunction(component);
                    }
                }
                else
                {
                    uiFunction(component);
                }

                ImGui::TreePop();
            }

            if (removeComponent)
            {
                if (s_DrawComponentCmdHistory && s_DrawComponentScene)
                {
                    s_DrawComponentCmdHistory->Execute(std::make_unique<RemoveComponentCommand<T>>(
                        s_DrawComponentScene, entity.GetUUID(), entity.GetComponent<T>()));
                }
                else
                {
                    entity.RemoveComponent<T>();
                }
            }
        }
    }

    static void DrawParticleEmissionSection(ParticleEmitter& emitter)
    {
        OLO_PROFILE_FUNCTION();

        if (!ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::DragFloat("Rate Over Time", &emitter.RateOverTime, 0.5f, 0.0f, 10000.0f);
        ImGui::DragFloat("Initial Speed", &emitter.InitialSpeed, 0.1f, 0.0f, 100.0f);
        ImGui::DragFloat("Speed Variance", &emitter.SpeedVariance, 0.1f, 0.0f, 50.0f);
        ImGui::DragFloat("Lifetime Min", &emitter.LifetimeMin, 0.05f, 0.01f, 100.0f);
        ImGui::DragFloat("Lifetime Max", &emitter.LifetimeMax, 0.05f, 0.01f, 100.0f);
        if (emitter.LifetimeMin > emitter.LifetimeMax)
            std::swap(emitter.LifetimeMin, emitter.LifetimeMax);
        ImGui::DragFloat("Initial Size", &emitter.InitialSize, 0.01f, 0.001f, 50.0f);
        ImGui::DragFloat("Size Variance", &emitter.SizeVariance, 0.01f, 0.0f, 25.0f);
        ImGui::DragFloat("Initial Rotation", &emitter.InitialRotation, 1.0f, -360.0f, 360.0f);
        ImGui::DragFloat("Rotation Variance", &emitter.RotationVariance, 1.0f, 0.0f, 360.0f);
        ImGui::ColorEdit4("Initial Color", glm::value_ptr(emitter.InitialColor));

        const char* shapeItems[] = { "Point", "Sphere", "Box", "Cone", "Ring", "Edge", "Mesh" };
        if (int shapeIdx = static_cast<int>(GetEmissionShapeType(emitter.Shape)); ImGui::Combo("Emission Shape", &shapeIdx, shapeItems, 7))
        {
            switch (static_cast<EmissionShapeType>(shapeIdx))
            {
                case EmissionShapeType::Point:
                    emitter.Shape = EmitPoint{};
                    break;
                case EmissionShapeType::Sphere:
                    emitter.Shape = EmitSphere{};
                    break;
                case EmissionShapeType::Box:
                    emitter.Shape = EmitBox{};
                    break;
                case EmissionShapeType::Cone:
                    emitter.Shape = EmitCone{};
                    break;
                case EmissionShapeType::Ring:
                    emitter.Shape = EmitRing{};
                    break;
                case EmissionShapeType::Edge:
                    emitter.Shape = EmitEdge{};
                    break;
                case EmissionShapeType::Mesh:
                {
                    EmitMesh m;
                    BuildEmitMeshFromPrimitive(m, 0);
                    emitter.Shape = std::move(m);
                    break;
                }
                default:
                    break;
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
            if (ring->InnerRadius > ring->OuterRadius)
                std::swap(ring->InnerRadius, ring->OuterRadius);
        }
        if (auto* edge = std::get_if<EmitEdge>(&emitter.Shape))
            ImGui::DragFloat("Edge Length", &edge->Length, 0.1f, 0.0f, 100.0f);
        if (auto* mesh = std::get_if<EmitMesh>(&emitter.Shape))
        {
            if (int primIdx = mesh->PrimitiveType; ImGui::Combo("Mesh Primitive", &primIdx, EmitMeshPrimitiveNames, EmitMeshPrimitiveCount))
            {
                BuildEmitMeshFromPrimitive(*mesh, primIdx);
            }
            ImGui::Text("Triangles: %u", static_cast<u32>(mesh->Triangles.size()));
        }
    }

    static void DrawParticleRenderingSection(ParticleSystemComponent& component, ParticleSystem& sys)
    {
        OLO_PROFILE_FUNCTION();

        if (!ImGui::CollapsingHeader("Rendering"))
            return;

        // Blend mode
        const char* blendModes[] = { "Alpha", "Additive", "Premultiplied Alpha" };
        if (int blendIdx = static_cast<int>(sys.BlendMode); ImGui::Combo("Blend Mode", &blendIdx, blendModes, 3))
            sys.BlendMode = static_cast<ParticleBlendMode>(blendIdx);

        // Render mode
        const char* renderModes[] = { "Billboard", "Stretched Billboard", "Mesh" };
        if (int renderIdx = static_cast<int>(sys.RenderMode); ImGui::Combo("Render Mode", &renderIdx, renderModes, 3))
            sys.RenderMode = static_cast<ParticleRenderMode>(renderIdx);

        ImGui::Checkbox("Depth Sort", &sys.DepthSortEnabled);

        // GPU compute simulation
        ImGui::Checkbox("GPU Simulation", &sys.UseGPU);
        if (sys.UseGPU)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(Billboard only)");
            if (auto* gpu = sys.GetGPUSystem())
            {
                ImGui::Text("GPU Alive: %u", gpu->GetAliveCount());
            }
        }

        ImGui::DragFloat("Velocity Inheritance", &sys.VelocityInheritance, 0.01f, 0.0f, 1.0f);

        // Soft particles
        ImGui::Checkbox("Soft Particles", &sys.SoftParticlesEnabled);
        if (sys.SoftParticlesEnabled)
        {
            ImGui::DragFloat("Soft Distance", &sys.SoftParticleDistance, 0.1f, 0.01f, 50.0f);
        }

        ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
        if (ImGui::BeginDragDropTarget())
        {
            if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            {
                std::filesystem::path texturePath = PathFromUtf8Payload(*payload);
                // Particle sprites are authored colour content.
                Ref<Texture2D> const texture = Texture2D::Create(texturePath.string(), /*srgb=*/true);
                if (texture && texture->IsLoaded())
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

        // Mesh selection (shown when render mode is Mesh)
        if (sys.RenderMode == ParticleRenderMode::Mesh)
        {
            ImGui::Text("Particle Mesh: %s", component.ParticleMesh ? "Assigned" : "None");
            ImGui::Button("Assign Mesh", ImVec2(100.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_MODEL"))
                {
                    std::filesystem::path meshPath = PathFromUtf8Payload(*payload);
                    auto model = Ref<Model>::Create(meshPath.string());
                    if (model && model->GetMeshCount() > 0)
                    {
                        auto meshSource = model->CreateCombinedMeshSource();
                        if (meshSource)
                            component.ParticleMesh = Ref<Mesh>::Create(meshSource);
                    }
                    else
                    {
                        OLO_WARN("Could not load mesh {0}", meshPath.filename().string());
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (component.ParticleMesh)
            {
                ImGui::SameLine();
                if (ImGui::Button("Clear Mesh"))
                    component.ParticleMesh = nullptr;
            }
        }
    }

    void SceneHierarchyPanel::DrawComponents(Entity entity)
    {
        // Set file-scope state for DrawComponent undo integration
        s_DrawComponentCmdHistory = m_CommandHistory;
        s_DrawComponentScene = m_Context;

        if (entity.HasComponent<TagComponent>())
        {
            auto& tag = entity.GetComponent<TagComponent>().Tag;

            char buffer[256];
            ::memset(buffer, 0, sizeof(buffer));
            std::strncpy(buffer, tag.c_str(), sizeof(buffer) - 1);

            // Save tag before InputText modifies it (for undo tracking)
            static std::string s_TagEditStart;
            const std::string tagBeforeInput = tag;

            if (ImGui::InputText("##Tag", buffer, sizeof(buffer)))
            {
                std::string oldTag = tag;
                tag = std::string(buffer);
                m_Context->UpdateEntityName(entity, oldTag, tag);
            }

            if (ImGui::IsItemActivated())
            {
                s_TagEditStart = tagBeforeInput;
            }

            if (ImGui::IsItemDeactivatedAfterEdit() && m_CommandHistory)
            {
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<RenameEntityCommand>(m_Context, entity.GetUUID(), s_TagEditStart, tag));
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
            DisplayAddComponentEntry<LuaScriptComponent>("Lua Script");
            DisplayAddComponentEntry<SpriteRendererComponent>("Sprite Renderer");
            DisplayAddComponentEntry<CircleRendererComponent>("Circle Renderer");
            DisplayAddComponentEntry<Rigidbody2DComponent>("Rigidbody 2D");
            DisplayAddComponentEntry<BoxCollider2DComponent>("Box Collider 2D");
            DisplayAddComponentEntry<CircleCollider2DComponent>("Circle Collider 2D");
            DisplayAddComponentEntry<TextComponent>("Text Component");
            DisplayAddComponentEntry<LocalizedTextComponent>("Localized Text");

            ImGui::Separator();

            // 3D Components
            DisplayAddComponentEntry<MeshComponent>("Mesh");
            DisplayAddComponentEntry<InstancedMeshComponent>("Instanced Mesh");
            DisplayAddComponentEntry<ModelComponent>("Model (with Materials)");
            DisplayAddComponentEntry<MaterialComponent>("Material");
            DisplayAddComponentEntry<LODGroupComponent>("LOD Group");
            DisplayAddComponentEntry<TileRendererComponent>("Tile Renderer");
            DisplayAddComponentEntry<DirectionalLightComponent>("Directional Light");
            DisplayAddComponentEntry<PointLightComponent>("Point Light");
            DisplayAddComponentEntry<SpotLightComponent>("Spot Light");
            DisplayAddComponentEntry<SphereAreaLightComponent>("Sphere Area Light");
            DisplayAddComponentEntry<EnvironmentMapComponent>("Environment Map (Skybox/IBL)");
            DisplayAddComponentEntry<ProceduralSkyComponent>("Procedural Sky (Preetham)");

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
            DisplayAddComponentEntry<AudioSoundGraphComponent>("Audio Sound Graph");

            ImGui::Separator();

            // Particle System
            DisplayAddComponentEntry<ParticleSystemComponent>("Particle System");

            if (!m_SelectionContext.HasComponent<ParticleSystemComponent>())
            {
                auto applyParticlePreset = [this](const char* label, void (*applyFn)(ParticleSystem&))
                {
                    if (ImGui::MenuItem(label))
                    {
                        if (m_CommandHistory)
                        {
                            m_CommandHistory->Execute(
                                std::make_unique<AddComponentWithInitCommand<ParticleSystemComponent>>(
                                    m_Context, m_SelectionContext.GetUUID(),
                                    [applyFn](ParticleSystemComponent& comp)
                                    { applyFn(comp.System); },
                                    std::string("Add ") + label));
                        }
                        else
                        {
                            auto& comp = m_SelectionContext.AddComponent<ParticleSystemComponent>();
                            applyFn(comp.System);
                        }
                        ImGui::CloseCurrentPopup();
                    }
                };

                applyParticlePreset("Snowfall Particle System", ParticlePresets::ApplySnowfall);
                applyParticlePreset("Blizzard Particle System", ParticlePresets::ApplyBlizzard);
                applyParticlePreset("Smoke Particle System", ParticlePresets::ApplySmoke);
                applyParticlePreset("Thick Smoke Particle System", ParticlePresets::ApplyThickSmoke);
                applyParticlePreset("Light Smoke Particle System", ParticlePresets::ApplyLightSmoke);
            }

            ImGui::Separator();

            // Terrain
            DisplayAddComponentEntry<TerrainComponent>("Terrain");
            DisplayAddComponentEntry<FoliageComponent>("Foliage");
            DisplayAddComponentEntry<SnowDeformerComponent>("Snow Deformer");
            DisplayAddComponentEntry<FogVolumeComponent>("Fog Volume");
            DisplayAddComponentEntry<DecalComponent>("Decal");
            DisplayAddComponentEntry<WaterComponent>("Water");

            ImGui::Separator();

            // Animation Components
            DisplayAddComponentEntry<AnimationStateComponent>("Animation State");
            DisplayAddComponentEntry<AnimationGraphComponent>("Animation Graph");
            DisplayAddComponentEntry<SkeletonComponent>("Skeleton");
            DisplayAddComponentEntry<SubmeshComponent>("Submesh");
            DisplayAddComponentEntry<MorphTargetComponent>("Morph Targets");

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
            DisplayAddComponentEntry<LightProbeComponent>("Light Probe");
            DisplayAddComponentEntry<LightProbeVolumeComponent>("Light Probe Volume");
            DisplayAddComponentEntry<ReflectionProbeComponent>("Reflection Probe");
            DisplayAddComponentEntry<StreamingVolumeComponent>("Streaming Volume");

            ImGui::Separator();

            // Networking
            DisplayAddComponentEntry<NetworkIdentityComponent>("Network Identity");

            ImGui::Separator();

            // Dialogue
            DisplayAddComponentEntry<DialogueComponent>("Dialogue");

            ImGui::Separator();

            // Navigation
            DisplayAddComponentEntry<NavMeshBoundsComponent>("NavMesh Bounds");
            DisplayAddComponentEntry<NavAgentComponent>("Nav Agent");

            ImGui::Separator();

            // AI
            DisplayAddComponentEntry<BehaviorTreeComponent>("Behavior Tree");
            DisplayAddComponentEntry<StateMachineComponent>("State Machine");

            ImGui::Separator();

            // Inventory
            DisplayAddComponentEntry<InventoryComponent>("Inventory");
            DisplayAddComponentEntry<ItemPickupComponent>("Item Pickup");
            DisplayAddComponentEntry<ItemContainerComponent>("Item Container");

            ImGui::Separator();

            // Quest
            DisplayAddComponentEntry<QuestJournalComponent>("Quest Journal");
            DisplayAddComponentEntry<QuestGiverComponent>("Quest Giver");

            ImGui::Separator();

            // Gameplay Ability System
            DisplayAddComponentEntry<AbilityComponent>("Gameplay Ability");

            ImGui::Separator();

            // Animation IK
            DisplayAddComponentEntry<IKTargetComponent>("IK Target");

            ImGui::EndPopup();
        }

        ImGui::PopItemWidth();

        DrawComponent<TransformComponent>("Transform", entity, [](auto& component)
                                          {
            DrawVec3Control("Translation", component.Translation);
            glm::vec3 rotation = glm::degrees(component.GetRotationEuler());
            glm::vec3 savedRotation = rotation;
            DrawVec3Control("Rotation", rotation);
            // Bit-exact edit detection — DragFloat3 returns the input bytes
            // verbatim when the user doesn't interact, so any byte difference
            // is a real user edit (cpp-coding-quality §2a; an epsilon compare
            // here would mask sub-degree edits the user actually intended).
            if (std::memcmp(&rotation, &savedRotation, sizeof(glm::vec3)) != 0)
            {
                component.SetRotationEuler(glm::radians(rotation));
            }
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
            }

            ImGui::Separator();
            ImGui::Checkbox("Runtime Control (FPS Fly)", &component.RuntimeControl);
            if (component.RuntimeControl)
            {
                ImGui::DragFloat("Fly Speed", &component.FlySpeed, 0.1f, 0.1f, 100.0f);
            } });

        DrawComponent<ScriptComponent>("Script", entity, [entity, scene = m_Context](auto& component) mutable
                                       {
            bool scriptClassExists = ScriptEngine::EntityClassExists(component.ClassName);

            // Dropdown picker from registered script classes
            {
                const auto& entityClasses = ScriptEngine::GetEntityClasses();
                static std::vector<std::string> cachedClassNames;
                if (static size_t cachedSize = 0; entityClasses.size() != cachedSize)
                {
                    cachedClassNames.clear();
                    cachedClassNames.reserve(entityClasses.size());
                    for (const auto& [name, _] : entityClasses)
                        cachedClassNames.push_back(name);
                    std::ranges::sort(cachedClassNames);
                    cachedSize = entityClasses.size();
                }

                const char* currentItem = component.ClassName.c_str();
                UI::ScopedStyleColor textColor(ImGuiCol_Text, ImVec4(0.9f, 0.2f, 0.3f, 1.0f), !scriptClassExists);

                if (ImGui::BeginCombo("Class", currentItem))
                {
                    for (const auto& name : cachedClassNames)
                    {
                        bool isSelected = (component.ClassName == name);
                        if (ImGui::Selectable(name.c_str(), isSelected))
                            component.ClassName = name;
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
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
                static std::unordered_map<std::string, f32> s_ScriptFieldSnapshots;
                    for (const auto& [name, field] : fields)
                    {
                        auto const snapshotKey = std::to_string(static_cast<u64>(entity.GetUUID())) + "::" + name;
                        // Field has been set in editor
                        if (entityFields.contains(name))
                        {
                            ScriptFieldInstance& scriptField = entityFields.at(name);

                            if (field.Type == ScriptFieldType::Float)
                            {
                                f32 data = scriptField.GetValue<f32>();
                                f32 preEditValue = data;
                                if (ImGui::DragFloat(name.c_str(), &data))
                                {
                                    scriptField.SetValue(data);
                                }
                                // Track undo for script field edits
                                if (s_DrawComponentCmdHistory && s_DrawComponentScene)
                                {
                                    if (ImGui::IsItemActivated())
                                    {
                                        s_ScriptFieldSnapshots[snapshotKey] = preEditValue;
                                    }
                                    if (ImGui::IsItemDeactivatedAfterEdit())
                                    {
                                        if (auto snapIt = s_ScriptFieldSnapshots.find(snapshotKey); snapIt != s_ScriptFieldSnapshots.end())
                                        {
                                            s_DrawComponentCmdHistory->PushAlreadyExecuted(
                                                std::make_unique<ScriptFieldChangeCommand>(
                                                    s_DrawComponentScene, entity.GetUUID(),
                                                    name, snapIt->second, data));
                                        }
                                    }
                                }
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
                                // Track undo for newly created script field
                                if (s_DrawComponentCmdHistory && s_DrawComponentScene && ImGui::IsItemDeactivatedAfterEdit())
                                {
                                    s_DrawComponentCmdHistory->PushAlreadyExecuted(
                                        std::make_unique<ScriptFieldChangeCommand>(
                                            s_DrawComponentScene, entity.GetUUID(),
                                            name, 0.0f, data));
                                }
                            }
                        }
                    }
                }
            } });

        DrawComponent<LuaScriptComponent>("Lua Script", entity, [](auto& component)
                                          {
            if (ImGui::InputText("Script File", &component.ScriptFile))
            {
            }
            ImGui::TextDisabled("Relative to project assets directory"); });

        DrawComponent<SpriteRendererComponent>("Sprite Renderer", entity, [](auto& component)
                                               {
            ImGui::ColorEdit4("Color", glm::value_ptr(component.Color));

            ImGui::Button("Texture", ImVec2(100.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
            if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    std::filesystem::path texturePath = PathFromUtf8Payload(*payload);
                    // Sprite art is authored colour, treat as sRGB.
                    Ref<Texture2D> const texture = Texture2D::Create(texturePath.string(), /*srgb=*/true);
                    if (texture && texture->IsLoaded())
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
                for (int i = 0; i < 3; ++i)
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
            ImGui::DragFloat("Line Spacing", &component.LineSpacing, 0.025f);
            ImGui::DragFloat("Max Width", &component.MaxWidth, 0.1f, 0.0f, 100.0f);
            ImGui::Checkbox("Drop Shadow", &component.DropShadow);
            if (component.DropShadow)
            {
                ImGui::DragFloat("Shadow Distance", &component.ShadowDistance, 0.001f, 0.0f, 1.0f);
                ImGui::ColorEdit4("Shadow Color", glm::value_ptr(component.ShadowColor));
            } });

        DrawComponent<LocalizedTextComponent>("Localized Text", entity, [](auto& component)
                                              {
            // Auto-localizes the entity's TextComponent.TextString. The
            // resolved string + a small "RESOLVED:" preview is shown so the
            // user can verify their key lookup without flipping locales.
            ImGui::InputText("Localization Key", &component.LocalizationKey);
            if (!component.LocalizationKey.empty())
            {
                // Gate Get() on HasKey() to avoid spamming the missing-key
                // accumulator every frame the inspector is open on a key
                // the active locale doesn't have. The inspector is a
                // diagnostic surface — its lookups shouldn't show up in
                // the editor panel's "missing keys at runtime" report.
                ImGui::TextDisabled("Locale: %s", LocalizationManager::GetCurrentLocale().c_str());
                if (LocalizationManager::HasKey(component.LocalizationKey))
                {
                    const std::string resolved = LocalizationManager::Get(component.LocalizationKey);
                    ImGui::TextWrapped("Resolved: %s", resolved.c_str());
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
                    ImGui::TextWrapped("Key missing in active locale (will show fallback at runtime).");
                    ImGui::PopStyleColor();
                }
            } });

        // 3D Components
        DrawComponent<MeshComponent>("Mesh", entity, [entity, scene = m_Context](auto& component) mutable
                                     {
            ImGui::Text("Mesh Source: %s", component.m_MeshSource ? "Loaded" : "None");

            if (component.m_MeshSource)
            {
                ImGui::Text("Submeshes: %d", component.m_MeshSource->GetSubmeshes().Num());
                ImGui::Text("Vertices: %d", component.m_MeshSource->GetVertices().Num());
                ImGui::Text("Shadow VA: %s", component.m_MeshSource->HasShadowVertexArray() ? "Yes" : "No");

                // On-demand mesh analysis via meshoptimizer
                if (ImGui::TreeNode("Mesh Analysis"))
                {
                    static MeshAnalysis s_CachedAnalysis;
                    static AssetHandle s_AnalyzedHandle{0};
                    static u64 s_AnalyzedGeneration = 0;
                    static const MeshSource* s_AnalyzedSource = nullptr;

                    AssetHandle currentHandle = component.m_MeshSource->GetHandle();
                    u64 currentGeneration = component.m_MeshSource->GetGeneration();
                    const MeshSource* currentSource = component.m_MeshSource.get();
                    if (s_AnalyzedSource != currentSource || s_AnalyzedGeneration != currentGeneration || s_AnalyzedHandle != currentHandle)
                    {
                        s_CachedAnalysis = MeshOptimization::AnalyzeMesh(*component.m_MeshSource);
                        s_AnalyzedSource = currentSource;
                        s_AnalyzedGeneration = currentGeneration;
                        s_AnalyzedHandle = currentHandle;
                    }
                    ImGui::Text("Triangles: %u", s_CachedAnalysis.TriangleCount);
                    ImGui::Text("ACMR: %.3f (lower=better)", s_CachedAnalysis.ACMR);
                    ImGui::Text("ATVR: %.3f (ideal=1.0)", s_CachedAnalysis.ATVR);
                    ImGui::Text("Overdraw: %.3f (ideal=1.0)", s_CachedAnalysis.Overdraw);
                    ImGui::Text("Fetch Overfetch: %.3f (ideal=1.0)", s_CachedAnalysis.OverfetchRatio);
                    ImGui::TreePop();
                }
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
                        // Track auto-added components for undo
                        auto* cmdHistory = s_DrawComponentCmdHistory;
                        auto cmdScene = s_DrawComponentScene;
                        UUID entityUUID = entity.GetUUID();
                        auto compound = std::make_unique<CompoundCommand>("Import Animated Model");

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
                                if (cmdHistory && cmdScene)
                                {
                                    compound->Add(std::make_unique<AddComponentCommand<MaterialComponent>>(cmdScene, entityUUID));
                                }
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
                                if (cmdHistory && cmdScene)
                                {
                                    compound->Add(std::make_unique<AddComponentCommand<SkeletonComponent>>(cmdScene, entityUUID));
                                }
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
                                if (cmdHistory && cmdScene)
                                {
                                    compound->Add(std::make_unique<AddComponentCommand<AnimationStateComponent>>(cmdScene, entityUUID));
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

                        // Push compound command for auto-added components (MeshComponent change handled by DrawComponent tracking)
                        if (cmdHistory && !compound->IsEmpty())
                        {
                            cmdHistory->PushAlreadyExecuted(std::move(compound));
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
                    component.m_Primitive = static_cast<MeshPrimitive>(currentPrimitive);
                }
                currentPrimitive = 0; // Reset selection
            }

            // Clear mesh button
            if (component.m_MeshSource)
            {
                if (ImGui::Button("Clear Mesh"))
                {
                    component.m_MeshSource.Reset();
                    component.m_Primitive = MeshPrimitive::None;
                }
            } });

        DrawComponent<InstancedMeshComponent>("Instanced Mesh", entity, [](auto& component)
                                              {
            // Phase 5 inspector: read-only summary of the component's resource
            // bindings + flag editors + a basic scatter-brush MVP for inline
            // placements. Procedural density / slope-aware surface scatter is
            // a dedicated viewport-tool feature — see
            // docs/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md §1 for the spec.
            // The volume-scatter controls below let an author drop N random
            // placements in an authored AABB via a single button — enough
            // for a working foliage demo without leaving the inspector.
            ImGui::Text("Mesh Source: %s", component.MeshSource ? "Loaded" : "None");
            if (component.MeshSource)
            {
                ImGui::Text("Submeshes: %d", component.MeshSource->GetSubmeshes().Num());
            }
            ImGui::Text("Override Material: %s", component.OverrideMaterial ? "Set" : "None");
            ImGui::Text("Inline Instance Count: %zu", component.Instances.size());
            ImGui::Text("Placement Asset Handle: %llu", static_cast<unsigned long long>(component.PlacementAssetHandle));
            ImGui::Separator();
            ImGui::Checkbox("Frustum Cull Per Instance", &component.FrustumCullPerInstance);
            ImGui::Checkbox("Cast Shadows", &component.CastShadows);
            ImGui::DragFloat("Cull Distance", &component.CullDistance, 1.0f, 0.0f, 100000.0f, "%.1f m");
            ImGui::TextDisabled("0 disables distance culling");

            ImGui::Separator();
            ImGui::TextUnformatted("Scatter Brush (MVP)");
            static i32 s_ScatterCount = 100;
            static glm::vec3 s_ScatterMin = glm::vec3(-10.0f, 0.0f, -10.0f);
            static glm::vec3 s_ScatterMax = glm::vec3( 10.0f, 0.0f,  10.0f);
            static f32 s_ScatterScaleMin = 0.8f;
            static f32 s_ScatterScaleMax = 1.2f;
            static bool s_ScatterRandomYRot = true;
            // Poisson-disc sampling (Bridson 2007) avoids the clumpy
            // clusters that uniform-random produces — important for
            // foliage scatter where uniform distribution looks artificial.
            // Disabled by default; when enabled the `Count` slider becomes
            // a *budget* (the algorithm stops earlier if the XZ footprint
            // can't fit `Count` non-overlapping discs).
            static bool s_ScatterUsePoisson = false;
            static f32 s_ScatterPoissonRadius = 1.5f;
            ImGui::SliderInt("Count", &s_ScatterCount, 1, 5000);
            ImGui::DragFloat3("AABB Min", &s_ScatterMin.x, 0.5f);
            ImGui::DragFloat3("AABB Max", &s_ScatterMax.x, 0.5f);
            ImGui::DragFloatRange2("Scale", &s_ScatterScaleMin, &s_ScatterScaleMax, 0.01f, 0.01f, 10.0f);
            ImGui::Checkbox("Random Y Rotation", &s_ScatterRandomYRot);
            ImGui::Checkbox("Poisson Disc (no clumps)", &s_ScatterUsePoisson);
            if (s_ScatterUsePoisson)
            {
                ImGui::DragFloat("Min Spacing (XZ)", &s_ScatterPoissonRadius, 0.05f, 0.05f, 50.0f, "%.2f m");
                ImGui::TextDisabled("Y is uniform-random inside [Min.y, Max.y]");
            }
            if (ImGui::Button("Scatter Append"))
            {
                // Deterministic RNG via seed-from-frame is too unstable for
                // editor authoring (re-clicking yields the same set), so we
                // pull entropy from std::random_device. Authored result lives
                // in the component's Instances list and is YAML-persisted
                // by SceneSerializer like any other inline placement.
                std::random_device rd;
                std::mt19937 rng(rd());
                std::uniform_real_distribution<f32> distY(s_ScatterMin.y, s_ScatterMax.y);
                std::uniform_real_distribution<f32> distScale(s_ScatterScaleMin, s_ScatterScaleMax);
                std::uniform_real_distribution<f32> distRot(0.0f, 6.2831853f);

                // Build the XZ position list. Two strategies:
                //   - Uniform-random: classic per-instance independent draws.
                //   - Poisson disc (Bridson 2007): grid-accelerated rejection
                //     so no two samples are closer than `r` on the XZ plane.
                std::vector<glm::vec2> xzPositions;
                xzPositions.reserve(static_cast<sizet>(s_ScatterCount));
                if (!s_ScatterUsePoisson)
                {
                    std::uniform_real_distribution<f32> distX(s_ScatterMin.x, s_ScatterMax.x);
                    std::uniform_real_distribution<f32> distZ(s_ScatterMin.z, s_ScatterMax.z);
                    for (i32 i = 0; i < s_ScatterCount; ++i)
                        xzPositions.emplace_back(distX(rng), distZ(rng));
                }
                else
                {
                    // Bridson 2007: 2D Poisson-disc sampling with a uniform
                    // grid for O(1) neighbour queries. `Count` acts as the
                    // budget — the algorithm stops either when the active
                    // list empties (footprint saturated) or when the budget
                    // is reached, whichever comes first.
                    const f32 r = std::max(0.05f, s_ScatterPoissonRadius);
                    const f32 minX = s_ScatterMin.x, maxX = s_ScatterMax.x;
                    const f32 minZ = s_ScatterMin.z, maxZ = s_ScatterMax.z;
                    if (maxX > minX && maxZ > minZ)
                    {
                        const f32 cellSize = r * 0.7071067811865475f; // r / sqrt(2)
                        const i32 cellsX = std::max(1, static_cast<i32>(std::ceil((maxX - minX) / cellSize)));
                        const i32 cellsZ = std::max(1, static_cast<i32>(std::ceil((maxZ - minZ) / cellSize)));
                        std::vector<i32> grid(static_cast<sizet>(cellsX * cellsZ), -1);
                        auto cellIndex = [&minX, &minZ, &cellSize, &cellsX, &cellsZ](f32 x, f32 z) -> std::pair<i32, i32>
                        {
                            i32 cx = std::clamp(static_cast<i32>((x - minX) / cellSize), 0, cellsX - 1);
                            i32 cz = std::clamp(static_cast<i32>((z - minZ) / cellSize), 0, cellsZ - 1);
                            return { cx, cz };
                        };

                        std::uniform_real_distribution<f32> distX(minX, maxX);
                        std::uniform_real_distribution<f32> distZ(minZ, maxZ);
                        std::uniform_real_distribution<f32> distAngle(0.0f, 6.2831853f);
                        std::uniform_real_distribution<f32> distRadiusFactor(1.0f, 2.0f);

                        // Seed: one uniformly chosen point.
                        const glm::vec2 seed{ distX(rng), distZ(rng) };
                        xzPositions.push_back(seed);
                        auto [scx, scz] = cellIndex(seed.x, seed.y);
                        grid[scz * cellsX + scx] = 0;
                        std::vector<i32> activeList = { 0 };

                        // k = 30 is Bridson's recommended sample-per-point
                        // budget — at higher k the algorithm produces a
                        // slightly denser packing but spends more time per
                        // candidate. 30 is the empirical knee.
                        constexpr i32 kCandidatesPerActive = 30;
                        while (!activeList.empty() && static_cast<i32>(xzPositions.size()) < s_ScatterCount)
                        {
                            std::uniform_int_distribution<sizet> distActive(0, activeList.size() - 1);
                            const sizet activeSlot = distActive(rng);
                            const glm::vec2 base = xzPositions[static_cast<sizet>(activeList[activeSlot])];
                            bool found = false;
                            for (i32 c = 0; c < kCandidatesPerActive; ++c)
                            {
                                const f32 angle = distAngle(rng);
                                const f32 radius = r * distRadiusFactor(rng);
                                const glm::vec2 cand{ base.x + std::cos(angle) * radius,
                                                       base.y + std::sin(angle) * radius };
                                if (cand.x < minX || cand.x > maxX || cand.y < minZ || cand.y > maxZ)
                                    continue;

                                // Reject if any neighbour in the 5x5 grid
                                // window is within `r`. 5x5 (not 3x3) is
                                // required because annulus candidates can
                                // sit at distance up to 2r from `base` and
                                // we need to check cells they might touch.
                                auto [ccx, ccz] = cellIndex(cand.x, cand.y);
                                bool tooClose = false;
                                for (i32 dz = -2; dz <= 2 && !tooClose; ++dz)
                                {
                                    for (i32 dx = -2; dx <= 2 && !tooClose; ++dx)
                                    {
                                        const i32 nx = ccx + dx;
                                        const i32 nz = ccz + dz;
                                        if (nx < 0 || nx >= cellsX || nz < 0 || nz >= cellsZ)
                                            continue;
                                        const i32 idx = grid[nz * cellsX + nx];
                                        if (idx < 0)
                                            continue;
                                        const glm::vec2 other = xzPositions[static_cast<sizet>(idx)];
                                        if (glm::distance(cand, other) < r)
                                            tooClose = true;
                                    }
                                }
                                if (tooClose)
                                    continue;

                                xzPositions.push_back(cand);
                                const i32 newIdx = static_cast<i32>(xzPositions.size()) - 1;
                                grid[ccz * cellsX + ccx] = newIdx;
                                activeList.push_back(newIdx);
                                found = true;
                                break;
                            }
                            if (!found)
                            {
                                // Saturated around this point — retire it.
                                activeList[activeSlot] = activeList.back();
                                activeList.pop_back();
                            }
                        }
                    }
                }

                component.Instances.reserve(component.Instances.size() + xzPositions.size());
                for (const auto& xz : xzPositions)
                {
                    InstanceData inst;
                    glm::vec3 pos(xz.x, distY(rng), xz.y);
                    f32 scale = distScale(rng);
                    f32 yaw = s_ScatterRandomYRot ? distRot(rng) : 0.0f;
                    glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
                    glm::mat4 r = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0));
                    glm::mat4 s = glm::scale(glm::mat4(1.0f), glm::vec3(scale));
                    inst.Transform = t * r * s;
                    inst.Normal = glm::transpose(glm::inverse(inst.Transform));
                    inst.PrevTransform = inst.Transform;
                    component.Instances.push_back(inst);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Inline"))
            {
                component.Instances.clear();
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

        DrawComponent<LODGroupComponent>("LOD Group", entity, [entity](auto& component)
                                         {
            ImGui::Checkbox("Enabled", &component.m_Enabled);
            ImGui::DragFloat("LOD Bias", &component.m_LODGroup.Bias, 0.01f, 0.01f, 10.0f, "%.2f");

            ImGui::Separator();
            ImGui::Text("LOD Levels: %zu", component.m_LODGroup.Levels.size());

            i32 removeIndex = -1;
            bool needsResort = false;
            for (sizet i = 0; i < component.m_LODGroup.Levels.size(); ++i)
            {
                auto& level = component.m_LODGroup.Levels[i];
                ImGui::PushID(static_cast<int>(i));

                ImGui::Text("LOD %zu", i);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
                if (ImGui::Button("X"))
                {
                    removeIndex = static_cast<i32>(i);
                }

                ImGui::Text("Mesh Handle: %llu", static_cast<unsigned long long>(level.MeshHandle));

                // Drag-drop target for mesh asset
                if (ImGui::BeginDragDropTarget())
                {
                    if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_MODEL"))
                    {
                        std::filesystem::path meshPath = PathFromUtf8Payload(*payload);
                        auto assetManager = Project::GetAssetManager().As<EditorAssetManager>();
                        if (assetManager)
                        {
                            AssetHandle handle = assetManager->ImportAsset(meshPath);
                            if (handle != 0)
                            {
                                level.MeshHandle = handle;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                f32 prevDistance = level.MaxDistance;
                ImGui::DragFloat("Max Distance", &level.MaxDistance, 1.0f, 0.0f, 100000.0f, "%.1f");

                if (level.MaxDistance != prevDistance)
                {
                    needsResort = true;
                }

                ImGui::Text("Triangles: %u", level.TriangleCount);

                ImGui::PopID();
                ImGui::Separator();
            }

            if (removeIndex >= 0)
            {
                component.m_LODGroup.Levels.erase(component.m_LODGroup.Levels.begin() + removeIndex);
            }

            if (needsResort && component.m_LODGroup.Levels.size() > 1)
            {
                std::sort(component.m_LODGroup.Levels.begin(), component.m_LODGroup.Levels.end(),
                          [](const LODLevel& a, const LODLevel& b) { return a.MaxDistance < b.MaxDistance; });
            }

            if (ImGui::Button("Add LOD Level"))
            {
                f32 nextDistance = component.m_LODGroup.Levels.empty()
                    ? 50.0f
                    : component.m_LODGroup.Levels.back().MaxDistance + 50.0f;
                component.m_LODGroup.Levels.emplace_back(AssetHandle{0}, nextDistance);
            }

            // Auto-generate LODs from the entity's mesh source
            if (entity.HasComponent<MeshComponent>())
            {
                auto& meshComp = entity.GetComponent<MeshComponent>();
                if (meshComp.m_MeshSource && meshComp.m_MeshSource->GetSubmeshes().Num() == 1)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Generate LODs"))
                    {
                        // Remove only previously generated (owned) LOD assets to avoid
                        // deleting shared or user-assigned memory-only assets.
                        for (const auto& handle : component.m_GeneratedLODHandles)
                        {
                            if (handle != 0 && AssetManager::IsMemoryAsset(handle))
                            {
                                AssetManager::RemoveAsset(handle);
                            }
                        }
                        component.m_GeneratedLODHandles.clear();

                        // Register the base mesh as a memory asset so LOD 0 has a valid handle
                        auto baseMesh = Ref<Mesh>::Create(meshComp.m_MeshSource, 0);
                        AssetHandle const baseHandle = AssetManager::AddMemoryOnlyAsset(baseMesh);
                        component.m_GeneratedLODHandles.push_back(baseHandle);

                        component.m_LODGroup = MeshOptimization::GenerateLODGroup(
                            *meshComp.m_MeshSource, baseHandle, 4, 200.0f);

                        // Track all generated LOD handles (skip LOD 0 which is the base)
                        for (sizet li = 1; li < component.m_LODGroup.Levels.size(); ++li)
                        {
                            component.m_GeneratedLODHandles.push_back(
                                component.m_LODGroup.Levels[li].MeshHandle);
                        }
                    }
                }
            } });

        DrawComponent<TileRendererComponent>("Tile Renderer", entity, [](auto& component)
                                             {
            // Tile mesh info
            ImGui::Text("Tile Mesh: %s", component.TileMesh ? "Assigned" : "None");

            // Drag-drop target for tile mesh
            if (ImGui::BeginDragDropTarget())
            {
                if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_MODEL"))
                {
                    std::filesystem::path meshPath = PathFromUtf8Payload(*payload);
                    auto model = Ref<Model>::Create(meshPath.string());
                    if (model && model->GetMeshCount() > 0)
                    {
                        auto combinedMeshSource = model->CreateCombinedMeshSource();
                        if (combinedMeshSource)
                        {
                            component.TileMesh = Ref<Mesh>::Create(combinedMeshSource, 0);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (component.TileMesh && ImGui::Button("Clear Mesh"))
            {
                component.TileMesh = nullptr;
            }

            ImGui::Separator();

            // Grid dimensions
            i32 width = static_cast<i32>(component.Width);
            i32 height = static_cast<i32>(component.Height);
            if (ImGui::DragInt("Width", &width, 1.0f, 1, 256))
            {
                component.ResizeGrid(static_cast<u32>(std::clamp(width, 1, 256)), component.Height);
            }
            if (ImGui::DragInt("Height", &height, 1.0f, 1, 256))
            {
                component.ResizeGrid(component.Width, static_cast<u32>(std::clamp(height, 1, 256)));
            }

            ImGui::DragFloat("Tile Size", &component.TileSize, 0.1f, 0.01f, 100.0f, "%.2f");

            ImGui::Separator();

            // Material palette
            ImGui::Text("Materials: %zu", component.Materials.size());

            i32 removeIdx = -1;
            auto materialCount = component.Materials.size();
            for (sizet i = 0; i < materialCount; ++i)
            {
                auto& mat = component.Materials[i];
                ImGui::PushID(static_cast<int>(i));

                ImGui::Text("Material %zu", i);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
                if (component.Materials.size() > 1 && ImGui::Button("X"))
                {
                    removeIdx = static_cast<i32>(i);
                }

                auto baseColor = mat.GetBaseColorFactor();
                if (glm::vec3 albedo(baseColor.r, baseColor.g, baseColor.b); ImGui::ColorEdit3("Albedo", glm::value_ptr(albedo)))
                {
                    mat.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
                }

                if (auto metallic = mat.GetMetallicFactor(); ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f, "%.2f"))
                {
                    mat.SetMetallicFactor(metallic);
                }

                if (auto roughness = mat.GetRoughnessFactor(); ImGui::DragFloat("Roughness", &roughness, 0.01f, 0.0f, 1.0f, "%.2f"))
                {
                    mat.SetRoughnessFactor(roughness);
                }

                ImGui::PopID();
                ImGui::Separator();
            }

            if (removeIdx >= 0)
            {
                u8 removedIdx = static_cast<u8>(removeIdx);
                component.Materials.erase(component.Materials.begin() + removeIdx);
                // Remap MaterialIDs: shift indices down, clamp deleted references
                for (auto& id : component.MaterialIDs)
                {
                    if (id == removedIdx)
                        id = 0;
                    else if (id > removedIdx)
                        --id;
                }
            }

            if (component.Materials.size() < 255 && ImGui::Button("Add Material"))
            {
                component.Materials.emplace_back();
            }

            ImGui::Separator();

            // Per-cell material ID editor
            auto expectedSize = static_cast<sizet>(component.Width) * component.Height;
            if (!component.Materials.empty() && component.Width > 0 && component.Height > 0
                && component.MaterialIDs.size() == expectedSize && ImGui::TreeNode("Tile Grid"))
            {
                auto maxIdx = static_cast<u8>(std::min<sizet>(component.Materials.size() - 1, 255));

                // Compute visible column range to avoid creating hundreds of off-screen widgets
                constexpr f32 cellWidgetWidth = 34.0f; // 30px widget + ~4px spacing
                auto visibleCols = std::max(1u, static_cast<u32>(ImGui::GetContentRegionAvail().x / cellWidgetWidth));
                static u32 colOffset = 0;
                if (visibleCols < component.Width)
                {
                    i32 off = static_cast<i32>(colOffset);
                    ImGui::SliderInt("Col Offset", &off, 0, static_cast<i32>(component.Width - visibleCols));
                    colOffset = static_cast<u32>(std::max(0, off));
                }
                else
                {
                    colOffset = 0;
                }
                // Clamp colOffset so it stays valid after grid resize
                u32 maxColOffset = (component.Width > visibleCols) ? (component.Width - visibleCols) : 0;
                colOffset = std::min(colOffset, maxColOffset);
                u32 colEnd = std::min(colOffset + visibleCols, component.Width);

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(component.Height));
                while (clipper.Step())
                {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                    {
                        ImGui::PushID(row);
                        for (u32 col = colOffset; col < colEnd; ++col)
                        {
                            if (col > colOffset)
                                ImGui::SameLine();

                            auto idx = static_cast<sizet>(row) * component.Width + col;
                            i32 matId = component.MaterialIDs[idx];
                            ImGui::PushID(static_cast<int>(col));
                            ImGui::SetNextItemWidth(30.0f);
                            if (ImGui::DragInt("##cell", &matId, 1.0f, 0, static_cast<i32>(maxIdx)))
                            {
                                component.MaterialIDs[idx] = static_cast<u8>(
                                    std::clamp(matId, 0, static_cast<i32>(maxIdx)));
                            }
                            ImGui::PopID();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::TreePop();
            } });

        DrawComponent<MaterialComponent>("Material", entity, [](auto& component)
                                         {
            // Material Presets Dropdown
            const char* presets[] = { "Custom", "Default", "Metallic", "Rough Plastic", "Polished Metal", "Rubber", "Glass", "Gold", "Silver", "Copper", "Wood", "Marble" };
            if (static int currentPreset = 0; ImGui::Combo("Preset", &currentPreset, presets, IM_ARRAYSIZE(presets)))
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
            if (glm::vec3 albedo(baseColor.r, baseColor.g, baseColor.b); ImGui::ColorEdit3("Albedo", glm::value_ptr(albedo)))
                component.m_Material.SetBaseColorFactor(glm::vec4(albedo, baseColor.a));

            if (f32 metallic = component.m_Material.GetMetallicFactor(); ImGui::DragFloat("Metallic", &metallic, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetMetallicFactor(metallic);

            if (f32 roughness = component.m_Material.GetRoughnessFactor(); ImGui::DragFloat("Roughness", &roughness, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRoughnessFactor(roughness);

            ImGui::Separator();

            // Shader Graph assignment
            {
                bool hasGraph = component.m_ShaderGraphHandle != 0;
                std::string currentLabel = hasGraph ? ("ShaderGraph: " + std::to_string(static_cast<u64>(component.m_ShaderGraphHandle))) : "None (Default PBR)";

                ImGui::Text("Shader Graph");
                ImGui::SameLine();

                ImGui::BeginDisabled();
                ImGui::Button(currentLabel.c_str());
                ImGui::EndDisabled();

                // Drop target for drag-and-drop from ContentBrowser
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_SHADERGRAPH"))
                    {
                        std::filesystem::path assetPath = PathFromUtf8Payload(*payload);
                        auto assetManager = Project::GetAssetManager().As<EditorAssetManager>();
                        if (assetManager)
                        {
                            AssetHandle handle = assetManager->ImportAsset(assetPath);
                            if (handle != 0 && AssetManager::GetAssetType(handle) == AssetType::ShaderGraph)
                            {
                                if (auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(handle))
                                {
                                    if (auto shader = graphAsset->CompileToShader("ShaderGraph_" + std::to_string(static_cast<u64>(handle))))
                                    {
                                        component.m_ShaderGraphHandle = handle;
                                        component.m_Material.SetShader(shader);
                                    }
                                    else
                                    {
                                        OLO_WARN("ShaderGraph {} failed to compile, not assigning to material", static_cast<u64>(handle));
                                    }
                                }
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (hasGraph)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Clear##ShaderGraph"))
                    {
                        component.m_ShaderGraphHandle = 0;
                        component.m_Material.SetShader(nullptr);
                    }

                    if (ImGui::Button("Recompile##ShaderGraph"))
                    {
                        if (auto graphAsset = AssetManager::GetAsset<ShaderGraphAsset>(component.m_ShaderGraphHandle))
                        {
                            graphAsset->MarkDirty();
                            if (auto shader = graphAsset->CompileToShader("ShaderGraph_" + std::to_string(static_cast<u64>(component.m_ShaderGraphHandle))))
                            {
                                component.m_Material.SetShader(shader);
                            }
                            else
                            {
                                OLO_WARN("ShaderGraph recompilation failed for handle {}", static_cast<u64>(component.m_ShaderGraphHandle));
                            }
                        }
                    }
                }
            } });

        DrawComponent<DirectionalLightComponent>("Directional Light", entity, [](auto& component)
                                                 {
            DrawVec3Control("Direction", component.m_Direction);
            ImGui::ColorEdit3("Color", glm::value_ptr(component.m_Color));
            ImGui::DragFloat("Intensity##DirectionalLight", &component.m_Intensity, 0.1f, 0.0f, 10.0f);
            ImGui::Checkbox("Cast Shadows##DirectionalLight", &component.m_CastShadows);
            if (component.m_CastShadows)
            {
                ImGui::Indent();
                ImGui::DragFloat("Shadow Bias##DirLight", &component.m_ShadowBias, 0.0001f, 0.0f, 0.05f, "%.4f");
                ImGui::DragFloat("Normal Bias##DirLight", &component.m_ShadowNormalBias, 0.001f, 0.0f, 0.1f, "%.3f");
                ImGui::DragFloat("Max Shadow Distance##DirLight", &component.m_MaxShadowDistance, 1.0f, 10.0f, 1000.0f);
                ImGui::DragFloat("Cascade Split Lambda##DirLight", &component.m_CascadeSplitLambda, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::Checkbox("Cascade Debug Visualization##DirLight", &component.m_CascadeDebugVisualization);
                ImGui::Unindent();
            } });

        DrawComponent<PointLightComponent>("Point Light", entity, [](auto& component)
                                           {
            ImGui::ColorEdit3("Color##PointLight", glm::value_ptr(component.m_Color));
            ImGui::DragFloat("Intensity##PointLight", &component.m_Intensity, 0.1f, 0.0f, 10.0f);
            ImGui::DragFloat("Range##PointLight", &component.m_Range, 0.1f, 0.1f, 100.0f);
            ImGui::DragFloat("Attenuation##PointLight", &component.m_Attenuation, 0.1f, 0.1f, 4.0f);
            ImGui::Checkbox("Cast Shadows##PointLight", &component.m_CastShadows);
            if (component.m_CastShadows)
            {
                ImGui::Indent();
                ImGui::DragFloat("Shadow Bias##PointLight", &component.m_ShadowBias, 0.0001f, 0.0f, 0.05f, "%.4f");
                ImGui::DragFloat("Normal Bias##PointLight", &component.m_ShadowNormalBias, 0.001f, 0.0f, 0.1f, "%.3f");
                ImGui::Unindent();
            } });

        DrawComponent<SpotLightComponent>("Spot Light", entity, [](auto& component)
                                          {
            DrawVec3Control("Direction##SpotLight", component.m_Direction);
            ImGui::ColorEdit3("Color##SpotLight", glm::value_ptr(component.m_Color));
            ImGui::DragFloat("Intensity##SpotLight", &component.m_Intensity, 0.1f, 0.0f, 10.0f);
            ImGui::DragFloat("Range##SpotLight", &component.m_Range, 0.1f, 0.1f, 100.0f);
            ImGui::DragFloat("Inner Cutoff##SpotLight", &component.m_InnerCutoff, 0.1f, 0.0f, 90.0f);
            ImGui::DragFloat("Outer Cutoff##SpotLight", &component.m_OuterCutoff, 0.1f, 0.0f, 90.0f);
            ImGui::DragFloat("Attenuation##SpotLight", &component.m_Attenuation, 0.1f, 0.1f, 4.0f);
            ImGui::Checkbox("Cast Shadows##SpotLight", &component.m_CastShadows);
            if (component.m_CastShadows)
            {
                ImGui::Indent();
                ImGui::DragFloat("Shadow Bias##SpotLight", &component.m_ShadowBias, 0.0001f, 0.0f, 0.05f, "%.4f");
                ImGui::DragFloat("Normal Bias##SpotLight", &component.m_ShadowNormalBias, 0.001f, 0.0f, 0.1f, "%.3f");
                ImGui::Unindent();
            } });

        DrawComponent<SphereAreaLightComponent>("Sphere Area Light", entity, [](auto& component)
                                                {
            ImGui::ColorEdit3("Color##SphereArea", glm::value_ptr(component.m_Color));
            ImGui::DragFloat("Intensity##SphereArea", &component.m_Intensity, 0.1f, 0.0f, 10.0f);
            ImGui::DragFloat("Radius##SphereArea", &component.m_Radius, 0.01f, 0.0f, 10.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Physical emitter radius. 0 collapses to a point light.");
            ImGui::DragFloat("Range##SphereArea", &component.m_Range, 0.1f, 0.0f, 100.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Falloff distance (lighting contribution clamps to zero beyond).");
            ImGui::Checkbox("Cast Shadows##SphereArea", &component.m_CastShadows);
            if (component.m_CastShadows)
            {
                ImGui::Indent();
                ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.3f, 1.0f),
                                   "Note: soft shadows from area lights are not implemented yet.");
                ImGui::Unindent();
            } });

        DrawComponent<ProceduralSkyComponent>("Procedural Sky", entity, [&entity](auto& component)
                                              {
            // Sun direction: edit as a normalised vec3 plus a "copy from
            // directional light" affordance so authoring matches DCC tools.
            glm::vec3 dir = component.m_SunDirection;
            if (ImGui::DragFloat3("Sun Direction##ProcSky", glm::value_ptr(dir), 0.01f, -1.0f, 1.0f))
            {
                component.m_SunDirection = dir;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toward-sun unit vector (world space). Re-normalised inside ComputeCoefficients; below-horizon directions are clamped to a small positive elevation.");

            ImGui::Checkbox("Link to Directional Light##ProcSky", &component.m_LinkSunToDirectionalLight);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("If on, the runtime overwrites the sun direction with -DirectionalLight.m_Direction each tick.");

            // Quick presets — useful for previewing time-of-day without manual
            // direction tweaking.
            if (ImGui::Button("Noon"))
            {
                component.m_SunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Mid-Morning"))
            {
                component.m_SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));
            }
            ImGui::SameLine();
            if (ImGui::Button("Sunset"))
            {
                component.m_SunDirection = glm::normalize(glm::vec3(0.95f, 0.15f, 0.25f));
            }

            ImGui::DragFloat("Turbidity##ProcSky", &component.m_Turbidity, 0.05f, 1.7f, 10.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Atmospheric haze. 2 = very clear / mountain, 3 = clear sky, 6 = hazy, 10 = thick haze. Outside [1.7, 10] the Preetham model degrades.");

            ImGui::DragFloat("Exposure##ProcSky", &component.m_Exposure, 0.005f, 0.0f, 2.0f, "%.3f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rate in the sky luminance tonemap 1-exp(-Exposure*Y). Higher = brighter, whiter sky; lower = darker, more saturated blue.");

            ImGui::Checkbox("Show Sun Disk##ProcSky", &component.m_ShowSunDisk);
            if (component.m_ShowSunDisk)
            {
                ImGui::Indent();
                ImGui::DragFloat("Sun Intensity##ProcSky", &component.m_SunIntensity, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Sun Disk Size##ProcSky", &component.m_SunDiskSize, 0.05f, 0.1f, 10.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Multiplier on the nominal solar angular radius (~0.27 deg).");
                ImGui::Unindent();
            }

            ImGui::Separator();
            ImGui::Checkbox("Enable Skybox##ProcSky", &component.m_EnableSkybox);
            ImGui::Checkbox("Enable IBL##ProcSky", &component.m_EnableIBL);
            if (component.m_EnableIBL)
            {
                ImGui::Indent();
                ImGui::DragFloat("IBL Intensity##ProcSky", &component.m_IBLIntensity, 0.01f, 0.0f, 5.0f);
                ImGui::Unindent();
            }

            // Resolution: ints in steps the cubemap pipeline likes.
            int res = static_cast<int>(component.m_CubemapResolution);
            const int kSteps[] = { 64, 128, 256, 512, 1024 };
            int currentIdx = 2;
            for (int i = 0; i < IM_ARRAYSIZE(kSteps); ++i)
            {
                if (res <= kSteps[i]) { currentIdx = i; break; }
            }
            if (ImGui::Combo("Cubemap Resolution##ProcSky", &currentIdx, "64\0""128\0""256\0""512\0""1024\0\0"))
            {
                component.m_CubemapResolution = static_cast<u32>(kSteps[currentIdx]);
                component.m_LastBakeHash = 0; // force rebake at the new resolution
                component.m_EnvironmentMap.Reset();
            }

            ImGui::Separator();
            if (ImGui::Button("Force Rebake##ProcSky"))
            {
                component.m_LastBakeHash = 0;
                component.m_EnvironmentMap.Reset();
            }
            ImGui::SameLine();
            if (component.m_EnvironmentMap)
            {
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "Baked");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.6f, 1.0f), "Not yet baked");
            } });

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

                // Diffuse generator selection. Drops the cached env map on
                // change so Scene::RenderScene3D regenerates it with the
                // freshly-selected IBLConfiguration on the next tick. The
                // IBLCache hashes UseSphericalHarmonics into its key, so
                // round-tripping between the two paths is cache-fast after
                // the first regen of each.
                if (ImGui::Checkbox("Use Spherical Harmonics (L2)##EnvMap", &component.m_UseSphericalHarmonics))
                {
                    component.m_EnvironmentMap = nullptr;
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Diffuse-irradiance generator:\n"
                        "  Off (default): Monte-Carlo cubemap convolution\n"
                        "                 (~1024 samples per output texel)\n"
                        "  On:            L2 spherical-harmonics projection\n"
                        "                 (9 coefficients, ~3-10x faster vs 1024-sample Monte-Carlo)\n\n"
                        "Outputs are binding/format compatible (same RGBA32F\n"
                        "irradiance cubemap at the same texture binding) — not\n"
                        "guaranteed bit-identical. SH-L2 captures the low-frequency\n"
                        "diffuse content; very sharp sky/ground transitions read\n"
                        "as slightly softer in the indirect diffuse term.");
                }
            } });

        DrawComponent<Rigidbody3DComponent>("Rigidbody 3D", entity, [](auto& component)
                                            {
            const char* bodyTypeStrings[] = { "Static", "Dynamic", "Kinematic" };
            if (const char* currentBodyTypeString = bodyTypeStrings[static_cast<int>(component.m_Type)]; ImGui::BeginCombo("Body Type", currentBodyTypeString))
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
            if (f32 staticFriction = component.m_Material.GetStaticFriction(); ImGui::DragFloat("Static Friction##BoxCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            if (f32 dynamicFriction = component.m_Material.GetDynamicFriction(); ImGui::DragFloat("Dynamic Friction##BoxCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##BoxCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<SphereCollider3DComponent>("Sphere Collider 3D", entity, [](auto& component)
                                                 {
            ImGui::DragFloat("Radius##SphereCollider3D", &component.m_Radius, 0.01f, 0.01f, 100.0f);
            DrawVec3Control("Offset##SphereCollider3D", component.m_Offset);
            if (f32 staticFriction = component.m_Material.GetStaticFriction(); ImGui::DragFloat("Static Friction##SphereCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            if (f32 dynamicFriction = component.m_Material.GetDynamicFriction(); ImGui::DragFloat("Dynamic Friction##SphereCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##SphereCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<CapsuleCollider3DComponent>("Capsule Collider 3D", entity, [](auto& component)
                                                  {
            ImGui::DragFloat("Radius##CapsuleCollider3D", &component.m_Radius, 0.01f, 0.01f, 100.0f);
            ImGui::DragFloat("Half Height##CapsuleCollider3D", &component.m_HalfHeight, 0.01f, 0.01f, 100.0f);
            DrawVec3Control("Offset##CapsuleCollider3D", component.m_Offset);
            if (f32 staticFriction = component.m_Material.GetStaticFriction(); ImGui::DragFloat("Static Friction##CapsuleCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            if (f32 dynamicFriction = component.m_Material.GetDynamicFriction(); ImGui::DragFloat("Dynamic Friction##CapsuleCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
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
            if (f32 staticFriction = component.m_Material.GetStaticFriction(); ImGui::DragFloat("Static Friction##MeshCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            if (f32 dynamicFriction = component.m_Material.GetDynamicFriction(); ImGui::DragFloat("Dynamic Friction##MeshCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
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
            if (int maxVertices = static_cast<int>(component.m_MaxVertices); ImGui::DragInt("Max Vertices##ConvexMeshCollider3D", &maxVertices, 1, 4, 256))
                component.m_MaxVertices = static_cast<u32>(maxVertices);
            if (f32 staticFriction = component.m_Material.GetStaticFriction(); ImGui::DragFloat("Static Friction##ConvexMeshCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            if (f32 dynamicFriction = component.m_Material.GetDynamicFriction(); ImGui::DragFloat("Dynamic Friction##ConvexMeshCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
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
            if (f32 staticFriction = component.m_Material.GetStaticFriction(); ImGui::DragFloat("Static Friction##TriangleMeshCollider3D", &staticFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetStaticFriction(staticFriction);
            if (f32 dynamicFriction = component.m_Material.GetDynamicFriction(); ImGui::DragFloat("Dynamic Friction##TriangleMeshCollider3D", &dynamicFriction, 0.01f, 0.0f, 2.0f))
                component.m_Material.SetDynamicFriction(dynamicFriction);
            f32 restitution = component.m_Material.GetRestitution();
            if (ImGui::DragFloat("Restitution##TriangleMeshCollider3D", &restitution, 0.01f, 0.0f, 1.0f))
                component.m_Material.SetRestitution(restitution); });

        DrawComponent<CharacterController3DComponent>("Character Controller 3D", entity, [](auto& component)
                                                      {
            ImGui::DragFloat("Slope Limit (deg)##CharacterController3D", &component.m_SlopeLimitDeg, 1.0f, 0.0f, 90.0f);
            ImGui::DragFloat("Step Offset##CharacterController3D", &component.m_StepOffset, 0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Jump Power##CharacterController3D", &component.m_JumpPower, 0.1f, 0.0f, 50.0f);
            if (int layerID = static_cast<int>(component.m_LayerID); ImGui::DragInt("Layer ID##CharacterController3D", &layerID, 1, 0, 31))
                component.m_LayerID = static_cast<u32>(layerID);
            ImGui::Checkbox("Disable Gravity##CharacterController3D", &component.m_DisableGravity);
            ImGui::Checkbox("Control Movement In Air##CharacterController3D", &component.m_ControlMovementInAir);
            ImGui::Checkbox("Control Rotation In Air##CharacterController3D", &component.m_ControlRotationInAir); });

        // Audio Components
        DrawComponent<AudioSourceComponent>("Audio Source", entity, [this](auto& component)
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
                if (int currentModel = static_cast<int>(component.Config.AttenuationModel); ImGui::Combo("Attenuation Model##AudioSource", &currentModel, attenuationModels, IM_ARRAYSIZE(attenuationModels)))
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

                ImGui::Separator();
                ImGui::Text("VBAP Panning");
                ImGui::SliderFloat("Spread##AudioSource", &component.Config.Spread, 0.0f, 1.0f, "%.3f");
                ImGui::SetItemTooltip("Virtual source spread [0..1]. 1.0 = full spread");
                ImGui::SliderFloat("Focus##AudioSource", &component.Config.Focus, 0.0f, 1.0f, "%.3f");
                ImGui::SetItemTooltip("Channel focus [0..1]. 1.0 = fully focused");
            }

            ImGui::Separator();
            ImGui::Text("DSP Filters");
            ImGui::SliderFloat("Low-Pass Cutoff##AudioSource", &component.Config.LowPassCutoff, 0.0f, 1.0f, "%.3f");
            ImGui::SetItemTooltip("Normalized cutoff [0..1]. 1.0 = 20 kHz (bypassed)");
            ImGui::SliderFloat("High-Pass Cutoff##AudioSource", &component.Config.HighPassCutoff, 0.0f, 1.0f, "%.3f");
            ImGui::SetItemTooltip("Normalized cutoff [0..1]. 0.0 = 20 Hz (bypassed)");
            ImGui::SliderFloat("Reverb Send##AudioSource", &component.Config.ReverbSend, 0.0f, 1.0f, "%.3f");
            ImGui::SetItemTooltip("Reverb send level [0..1]");

            ImGui::Separator();
            ImGui::Text("Event System");
            if (auto prev = component.UseEventSystem; ImGui::Checkbox("Use Event System##AudioSource", &component.UseEventSystem))
            {
                if (component.UseEventSystem && !prev)
                {
                    if (auto* reg = m_Context->GetAudioCommandRegistry())
                    {
                        if (auto resolved = Audio::CommandID::FromString(component.StartEvent); reg->Contains(resolved))
                        {
                            component.StartCommandID = resolved;
                        }
                        else
                        {
                            component.StartCommandID = {};
                        }
                    }
                    else
                    {
                        component.StartCommandID = {};
                    }
                }
            }
            if (component.UseEventSystem)
            {
                char eventBuf[256] = {};
                std::strncpy(eventBuf, component.StartEvent.c_str(), sizeof(eventBuf) - 1);
                if (ImGui::InputText("Start Event##AudioSource", eventBuf, sizeof(eventBuf)))
                {
                    component.StartEvent = eventBuf;
                    if (auto* reg = m_Context->GetAudioCommandRegistry())
                    {
                        if (auto resolved = Audio::CommandID::FromString(component.StartEvent); reg->Contains(resolved))
                        {
                            component.StartCommandID = resolved;
                        }
                        else
                        {
                            component.StartCommandID = {};
                        }
                    }
                    else
                    {
                        component.StartCommandID = {};
                    }
                }

                // Validate against registry if available; re-resolve stale IDs
                bool validated = false;
                if (auto* reg = m_Context->GetAudioCommandRegistry())
                {
                    if (!component.StartEvent.empty() && !component.StartCommandID.IsValid())
                    {
                        if (auto resolved = Audio::CommandID::FromString(component.StartEvent); reg->Contains(resolved))
                        {
                            component.StartCommandID = resolved;
                        }
                    }
                    if (component.StartCommandID.IsValid())
                    {
                        validated = reg->Contains(component.StartCommandID);
                    }
                }

                if (component.StartEvent.empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No event name set");
                }
                else if (!component.StartCommandID.IsValid())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Unresolved start event");
                }
                else if (!validated && m_Context->IsRunning())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "CommandID: %u (not found in registry)", component.StartCommandID.ID);
                }
                else
                {
                    ImGui::Text("CommandID: %u", component.StartCommandID.ID);
                }
            } });

        DrawComponent<AudioListenerComponent>("Audio Listener", entity, [](auto& component)
                                              {
            ImGui::Checkbox("Active##AudioListener", &component.Active);

            ImGui::Separator();
            ImGui::Text("Cone Settings");
            ImGui::DragFloat("Inner Angle##AudioListener", &component.Config.ConeInnerAngle, 1.0f, 0.0f, 360.0f);
            ImGui::DragFloat("Outer Angle##AudioListener", &component.Config.ConeOuterAngle, 1.0f, 0.0f, 360.0f);
            ImGui::DragFloat("Outer Gain##AudioListener", &component.Config.ConeOuterGain, 0.01f, 0.0f, 1.0f); });

        DrawComponent<AudioSoundGraphComponent>("Audio Sound Graph", entity, [](auto& component)
                                                {
            // Asset display + drag-drop target. Until a content browser SOUNDGRAPH payload
            // type lands, we accept the generic CONTENT_BROWSER_ITEM and filter on AssetType
            // after import so dropping the wrong file produces a warning instead of silent
            // misbinding.
            std::string handleLabel = component.SoundGraphHandle != 0
                ? "Asset: " + std::to_string(static_cast<u64>(component.SoundGraphHandle))
                : "Asset: <none — drag a .olosoundgraph here>";
            ImGui::Button(handleLabel.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    std::filesystem::path assetPath = PathFromUtf8Payload(*payload);
                    if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                    {
                        AssetHandle handle = assetManager->ImportAsset(assetPath);
                        if (handle != 0 && AssetManager::GetAssetType(handle) == AssetType::SoundGraph)
                        {
                            component.SoundGraphHandle = handle;
                        }
                        else if (handle != 0)
                        {
                            OLO_WARN("Drag-dropped asset is not a SoundGraph (type: {0})",
                                     AssetUtils::AssetTypeToString(AssetManager::GetAssetType(handle)));
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (component.SoundGraphHandle != 0)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##SoundGraph"))
                {
                    component.SoundGraphHandle = 0;
                }
            }

            ImGui::Separator();
            ImGui::DragFloat("Volume##SoundGraph", &component.VolumeMultiplier, 0.01f, 0.0f, 4.0f);
            ImGui::DragFloat("Pitch##SoundGraph", &component.PitchMultiplier, 0.01f, 0.1f, 4.0f);
            ImGui::Checkbox("Looping##SoundGraph", &component.Looping);
            ImGui::Checkbox("Play On Awake##SoundGraph", &component.PlayOnAwake);

            ImGui::Separator();
            ImGui::Text("Runtime Sound: %s", component.Sound ? "Active" : "Not playing"); });

        // Animation Components
        DrawComponent<AnimationStateComponent>("Animation State", entity, [](auto& component)
                                               {
            const char* stateStrings[] = { "Idle", "Bounce", "Custom" };
            if (int currentState = static_cast<int>(component.m_State); ImGui::Combo("State##AnimationState", &currentState, stateStrings, IM_ARRAYSIZE(stateStrings)))
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

        DrawComponent<AnimationGraphComponent>("Animation Graph", entity, [](auto& component)
                                               {
            ImGui::Text("Asset Handle: %llu", static_cast<unsigned long long>(component.AnimationGraphAssetHandle));
            ImGui::Text("Runtime Graph: %s", component.RuntimeGraph ? "Loaded" : "None");
            if (component.RuntimeGraph)
            {
                ImGui::Text("Layers: %zu", component.RuntimeGraph->Layers.size());
                ImGui::Text("Current State: %s", component.RuntimeGraph->GetCurrentStateName().c_str());
            }

            ImGui::Separator();
            ImGui::Text("Parameters: %zu", component.Parameters.GetAll().size());
            for (auto const& [name, param] : component.Parameters.GetAll())
            {
                ImGui::BulletText("%s", name.c_str());
            } });

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
            if (int submeshIndex = static_cast<int>(component.m_SubmeshIndex); ImGui::DragInt("Submesh Index##Submesh", &submeshIndex, 1, 0, 255))
                component.m_SubmeshIndex = static_cast<u32>(submeshIndex);
            ImGui::Checkbox("Visible##Submesh", &component.m_Visible);
            ImGui::Text("Bone Entities: %zu", component.m_BoneEntityIds.size()); });

        DrawComponent<MorphTargetComponent>("Morph Targets", entity, [](auto& component)
                                            {
            if (!component.MorphTargets || component.MorphTargets->GetTargetCount() == 0)
            {
                ImGui::TextDisabled("No morph targets loaded");
                return;
            }

            ImGui::Text("Targets: %u", component.MorphTargets->GetTargetCount());
            ImGui::Separator();

            for (const auto& target : component.MorphTargets->Targets)
            {
                f32 weight = component.GetWeight(target.Name);
                if (ImGui::SliderFloat(target.Name.c_str(), &weight, 0.0f, 1.0f))
                {
                    component.SetWeight(target.Name, weight);
                }
            }

            ImGui::Separator();
            if (ImGui::Button("Reset All##MorphTargets"))
            {
                component.ResetAllWeights();
            }

            // Expression preset dropdown
            auto exprNames = FacialExpressionLibrary::GetExpressionNames();
            if (!exprNames.empty())
            {
                ImGui::Separator();
                ImGui::Text("Expression Presets");

                static int selectedExpr = -1;
                static f32 blendFactor = 1.0f;

                const char* preview = (selectedExpr >= 0 && selectedExpr < static_cast<int>(exprNames.size()))
                    ? exprNames[selectedExpr].c_str() : "Select...";

                if (ImGui::BeginCombo("Preset##ExprPreset", preview))
                {
                    for (int i = 0; i < static_cast<int>(exprNames.size()); ++i)
                    {
                        bool isSelected = (selectedExpr == i);
                        if (ImGui::Selectable(exprNames[i].c_str(), isSelected))
                            selectedExpr = i;
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::SliderFloat("Blend##ExprBlend", &blendFactor, 0.0f, 1.0f);

                if (selectedExpr >= 0 && selectedExpr < static_cast<int>(exprNames.size()))
                {
                    if (ImGui::Button("Apply Preset##ExprApply"))
                    {
                        FacialExpressionLibrary::ApplyExpression(component, exprNames[selectedExpr], blendFactor);
                    }
                }
            } });

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
                    std::filesystem::path texturePath = PathFromUtf8Payload(*payload);
                    // UI images are authored colour, treat as sRGB.
                    Ref<Texture2D> texture = Texture2D::Create(texturePath.string(), /*srgb=*/true);
                    if (texture && texture->IsLoaded())
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
            if (int charLimit = component.m_CharacterLimit; ImGui::DragInt("Character Limit", &charLimit, 1, 0, 10000))
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
            if (int selectedIndex = component.m_SelectedIndex; ImGui::DragInt("Selected Index", &selectedIndex, 1, -1, static_cast<int>(component.m_Options.size()) - 1))
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

            if (int maxP = static_cast<int>(sys.GetMaxParticles()); ImGui::DragInt("Max Particles", &maxP, 10, 1, 100000))
                sys.SetMaxParticles(static_cast<u32>(maxP));
            ImGui::Text("Alive: %u", sys.GetAliveCount());

            const char* spaceItems[] = { "Local", "World" };
            if (int spaceIdx = static_cast<int>(sys.SimulationSpace); ImGui::Combo("Simulation Space", &spaceIdx, spaceItems, 2))
                sys.SimulationSpace = static_cast<ParticleSpace>(spaceIdx);

            // Emission
            DrawParticleEmissionSection(emitter);

            // Texture
            DrawParticleRenderingSection(component, sys);

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
                    if (int sheetIdx = static_cast<int>(sys.TextureSheetModule.Mode); ImGui::Combo("Animation Mode", &sheetIdx, sheetModes, 2))
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
                    ImGui::DragFloat3("Linear Acceleration", glm::value_ptr(sys.VelocityModule.LinearAcceleration), 0.1f);
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
                if (int modeIdx = static_cast<int>(sys.CollisionModule.Mode); ImGui::Combo("Collision Mode", &modeIdx, collisionModes, 2))
                    sys.CollisionModule.Mode = static_cast<CollisionMode>(modeIdx);
                if (sys.CollisionModule.Mode == CollisionMode::WorldPlane)
                {
                    if (ImGui::DragFloat3("Plane Normal", glm::value_ptr(sys.CollisionModule.PlaneNormal), 0.01f, -1.0f, 1.0f))
                    {
                        f32 len = glm::length(sys.CollisionModule.PlaneNormal);
                        sys.CollisionModule.PlaneNormal = (len > 0.0001f) ? sys.CollisionModule.PlaneNormal / len : glm::vec3(0.0f, 1.0f, 0.0f);
                    }
                    ImGui::DragFloat("Plane Offset", &sys.CollisionModule.PlaneOffset, 0.1f);
                }
                ImGui::DragFloat("Bounce", &sys.CollisionModule.Bounce, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Lifetime Loss", &sys.CollisionModule.LifetimeLoss, 0.01f, 0.0f, 1.0f);
                ImGui::Checkbox("Kill On Collide", &sys.CollisionModule.KillOnCollide);
            }
            if (ImGui::CollapsingHeader("Force Fields"))
            {
                const char* ffTypes[] = { "Attraction", "Repulsion", "Vortex" };
                for (sizet fi = 0; fi < sys.ForceFields.size(); ++fi)
                {
                    auto& ff = sys.ForceFields[fi];
                    ImGui::PushID(static_cast<int>(fi));
                    if (std::string label = "Force Field " + std::to_string(fi); ImGui::TreeNode(label.c_str()))
                    {
                        ImGui::Checkbox("Enabled", &ff.Enabled);
                        if (int ffIdx = static_cast<int>(ff.Type); ImGui::Combo("Force Type", &ffIdx, ffTypes, 3))
                            ff.Type = static_cast<ForceFieldType>(ffIdx);
                        ImGui::DragFloat3("Position", glm::value_ptr(ff.Position), 0.1f);
                        ImGui::DragFloat("Strength", &ff.Strength, 0.1f, 0.0f, 1000.0f);
                        ImGui::DragFloat("Radius", &ff.Radius, 0.1f, 0.01f, 1000.0f);
                        if (ff.Type == ForceFieldType::Vortex)
                        {
                            if (ImGui::DragFloat3("Vortex Axis", glm::value_ptr(ff.Axis), 0.01f, -1.0f, 1.0f))
                            {
                                f32 len = glm::length(ff.Axis);
                                ff.Axis = (len > 0.0001f) ? ff.Axis / len : glm::vec3(0.0f, 1.0f, 0.0f);
                            }
                        }
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
                if (int maxPts = static_cast<int>(sys.TrailModule.MaxTrailPoints); ImGui::DragInt("Max Trail Points", &maxPts, 1, 2, 128))
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
                ImGui::DragFloat("LOD Max Distance", &sys.LODMaxDistance, 1.0f, 0.0f, 10000.0f);
                constexpr f32 epsilon = 1e-4f;
                sys.LODDistance1 = std::clamp(sys.LODDistance1, 0.0f, std::max(0.0f, sys.LODMaxDistance - epsilon));
            } });

        DrawComponent<TerrainComponent>("Terrain", entity, [](auto& component)
                                        {
            // Heightmap path
            char buf[256];
            std::strncpy(buf, component.m_HeightmapPath.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (ImGui::InputText("Heightmap Path", buf, sizeof(buf)))
                component.m_HeightmapPath = buf;

            // ── Procedural Generation ────────────────────────────────────
            ImGui::Separator();
            ImGui::Text("Procedural Generation");
            if (ImGui::Checkbox("Procedural Enabled", &component.m_ProceduralEnabled))
                component.m_NeedsRebuild = true;

            if (component.m_ProceduralEnabled)
            {
                if (ImGui::DragInt("Seed", &component.m_ProceduralSeed, 1))
                    component.m_NeedsRebuild = true;

                if (int procRes = static_cast<int>(component.m_ProceduralResolution); ImGui::DragInt("Resolution", &procRes, 1, 64, 2048))
                {
                    component.m_ProceduralResolution = static_cast<u32>(procRes);
                    component.m_NeedsRebuild = true;
                }

                if (int procOctaves = static_cast<int>(component.m_ProceduralOctaves); ImGui::DragInt("Octaves", &procOctaves, 1, 1, 12))
                {
                    component.m_ProceduralOctaves = static_cast<u32>(procOctaves);
                    component.m_NeedsRebuild = true;
                }

                if (ImGui::DragFloat("Frequency", &component.m_ProceduralFrequency, 0.1f, 0.1f, 20.0f))
                    component.m_NeedsRebuild = true;
                if (ImGui::DragFloat("Lacunarity", &component.m_ProceduralLacunarity, 0.05f, 1.0f, 4.0f))
                    component.m_NeedsRebuild = true;
                if (ImGui::DragFloat("Persistence", &component.m_ProceduralPersistence, 0.01f, 0.1f, 0.9f))
                    component.m_NeedsRebuild = true;

                if (ImGui::Button("Randomize Seed"))
                {
                    component.m_ProceduralSeed = RandomUtils::Int32(0, std::numeric_limits<i32>::max());
                    component.m_TerrainData = nullptr;
                    component.m_NeedsRebuild = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Regenerate"))
                {
                    component.m_TerrainData = nullptr;
                    component.m_NeedsRebuild = true;
                }
            }

            // World dimensions
            ImGui::DragFloat("World Size X", &component.m_WorldSizeX, 1.0f, 1.0f, 16384.0f);
            ImGui::DragFloat("World Size Z", &component.m_WorldSizeZ, 1.0f, 1.0f, 16384.0f);
            ImGui::DragFloat("Height Scale", &component.m_HeightScale, 0.5f, 0.0f, 1024.0f);

            // LOD / Tessellation settings
            ImGui::Separator();
            ImGui::Text("LOD & Tessellation");
            if (ImGui::Checkbox("Tessellation Enabled", &component.m_TessellationEnabled))
                component.m_NeedsRebuild = true;
            ImGui::DragFloat("Target Triangle Size (px)", &component.m_TargetTriangleSize, 0.5f, 1.0f, 64.0f);
            ImGui::DragFloat("Morph Region", &component.m_MorphRegion, 0.01f, 0.0f, 1.0f);

            // ── Material Layers ──────────────────────────────────────────
            ImGui::Separator();
            ImGui::Text("Material Layers");

            // Create material if it doesn't exist
            if (!component.m_Material)
            {
                if (ImGui::Button("Create Material"))
                {
                    component.m_Material = Ref<TerrainMaterial>::Create();
                    component.m_MaterialNeedsRebuild = true;
                }
            }
            else
            {
                auto& mat = component.m_Material;

                // Splatmap paths
                {
                    char sp0Buf[256]{};
                    std::strncpy(sp0Buf, mat->GetSplatmapPath(0).c_str(), sizeof(sp0Buf) - 1);
                    if (ImGui::InputText("Splatmap 0", sp0Buf, sizeof(sp0Buf)))
                    {
                        mat->SetSplatmapPath(0, sp0Buf);
                        component.m_MaterialNeedsRebuild = true;
                    }

                    char sp1Buf[256]{};
                    std::strncpy(sp1Buf, mat->GetSplatmapPath(1).c_str(), sizeof(sp1Buf) - 1);
                    if (ImGui::InputText("Splatmap 1", sp1Buf, sizeof(sp1Buf)))
                    {
                        mat->SetSplatmapPath(1, sp1Buf);
                        component.m_MaterialNeedsRebuild = true;
                    }
                }

                // Layer list
                u32 layerCount = mat->GetLayerCount();
                for (u32 i = 0; i < layerCount; ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    auto& layer = mat->GetLayer(i);

                    bool layerOpen = ImGui::TreeNodeEx(
                        layer.Name.c_str(),
                        ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_DefaultOpen);

                    // Remove button on same line
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
                    if (ImGui::SmallButton("X"))
                    {
                        mat->RemoveLayer(i);
                        component.m_MaterialNeedsRebuild = true;
                        if (layerOpen) ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }

                    if (layerOpen)
                    {
                        char nameBuf[128]{};
                        std::strncpy(nameBuf, layer.Name.c_str(), sizeof(nameBuf) - 1);
                        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
                            layer.Name = nameBuf;

                        char albBuf[256]{};
                        std::strncpy(albBuf, layer.AlbedoPath.c_str(), sizeof(albBuf) - 1);
                        if (ImGui::InputText("Albedo", albBuf, sizeof(albBuf)))
                        {
                            layer.AlbedoPath = albBuf;
                            component.m_MaterialNeedsRebuild = true;
                        }

                        char nrmBuf[256]{};
                        std::strncpy(nrmBuf, layer.NormalPath.c_str(), sizeof(nrmBuf) - 1);
                        if (ImGui::InputText("Normal", nrmBuf, sizeof(nrmBuf)))
                        {
                            layer.NormalPath = nrmBuf;
                            component.m_MaterialNeedsRebuild = true;
                        }

                        char armBuf[256]{};
                        std::strncpy(armBuf, layer.ARMPath.c_str(), sizeof(armBuf) - 1);
                        if (ImGui::InputText("ARM", armBuf, sizeof(armBuf)))
                        {
                            layer.ARMPath = armBuf;
                            component.m_MaterialNeedsRebuild = true;
                        }

                        ImGui::DragFloat("Tiling", &layer.TilingScale, 0.1f, 0.1f, 200.0f);
                        ImGui::DragFloat("Blend Sharpness", &layer.HeightBlendSharpness, 0.1f, 0.1f, 32.0f);
                        ImGui::DragFloat("Triplanar Sharpness", &layer.TriplanarSharpness, 0.1f, 1.0f, 32.0f);
                        ImGui::ColorEdit3("Base Color", &layer.BaseColor.x);
                        ImGui::DragFloat("Roughness", &layer.Roughness, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat("Metallic", &layer.Metallic, 0.01f, 0.0f, 1.0f);

                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }

                // Add layer button
                if (layerCount < MAX_TERRAIN_LAYERS)
                {
                    if (ImGui::Button("+ Add Layer"))
                    {
                        TerrainLayer newLayer;
                        newLayer.Name = "Layer " + std::to_string(layerCount);
                        mat->AddLayer(newLayer);
                        component.m_MaterialNeedsRebuild = true;
                    }
                }

                // Build / Rebuild materials button
                if (ImGui::Button("Build Materials"))
                {
                    component.m_MaterialNeedsRebuild = true;
                }

                if (mat->IsBuilt())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "Built (%u layers)", layerCount);
                }
            }

            // Rebuild button
            if (ImGui::Button("Rebuild Terrain"))
                component.m_NeedsRebuild = true;

            // ── Streaming Settings ───────────────────────────────────────
            ImGui::Separator();
            ImGui::Text("Streaming");
            if (ImGui::Checkbox("Streaming Enabled", &component.m_StreamingEnabled))
                component.m_NeedsRebuild = true;

            if (component.m_StreamingEnabled)
            {
                char tileDirBuf[256]{};
                std::strncpy(tileDirBuf, component.m_TileDirectory.c_str(), sizeof(tileDirBuf) - 1);
                if (ImGui::InputText("Tile Directory", tileDirBuf, sizeof(tileDirBuf)))
                {
                    component.m_TileDirectory = tileDirBuf;
                    component.m_NeedsRebuild = true;
                }

                char tilePatBuf[128]{};
                std::strncpy(tilePatBuf, component.m_TileFilePattern.c_str(), sizeof(tilePatBuf) - 1);
                if (ImGui::InputText("Tile File Pattern", tilePatBuf, sizeof(tilePatBuf)))
                {
                    component.m_TileFilePattern = tilePatBuf;
                    component.m_NeedsRebuild = true;
                }

                if (ImGui::DragFloat("Tile World Size", &component.m_TileWorldSize, 1.0f, 32.0f, 4096.0f))
                    component.m_NeedsRebuild = true;

                if (int tileRes = static_cast<int>(component.m_TileResolution); ImGui::DragInt("Tile Resolution", &tileRes, 1, 65, 2049))
                {
                    component.m_TileResolution = static_cast<u32>(tileRes);
                    component.m_NeedsRebuild = true;
                }

                if (int loadRadius = static_cast<int>(component.m_StreamingLoadRadius); ImGui::DragInt("Load Radius (tiles)", &loadRadius, 1, 1, 10))
                {
                    component.m_StreamingLoadRadius = static_cast<u32>(loadRadius);
                    component.m_NeedsRebuild = true;
                }

                if (int maxTiles = static_cast<int>(component.m_StreamingMaxTiles); ImGui::DragInt("Max Loaded Tiles", &maxTiles, 1, 1, 100))
                {
                    component.m_StreamingMaxTiles = static_cast<u32>(maxTiles);
                    component.m_NeedsRebuild = true;
                }

                // Streaming runtime info
                if (component.m_Streamer)
                {
                    ImGui::Separator();
                    ImGui::Text("Loaded Tiles: %u", component.m_Streamer->GetLoadedTileCount());
                    ImGui::Text("Loading: %u", component.m_Streamer->GetLoadingTileCount());
                }
            }

            // ── Voxel Override Settings ───────────────────────────────────
            ImGui::Separator();
            ImGui::Text("Voxel Override (Caves/Overhangs)");
            ImGui::Checkbox("Voxel Enabled", &component.m_VoxelEnabled);

            if (component.m_VoxelEnabled)
            {
                ImGui::DragFloat("Voxel Size", &component.m_VoxelSize, 0.1f, 0.25f, 4.0f, "%.2f");

                if (component.m_VoxelOverride)
                {
                    ImGui::Text("Voxel Chunks: %u", component.m_VoxelOverride->GetChunkCount());
                    ImGui::Text("Voxel Meshes: %u", static_cast<u32>(component.m_VoxelMeshes.size()));
                }
            }

            // Runtime info
            if (component.m_ChunkManager)
            {
                ImGui::Separator();
                ImGui::Text("Chunks: %u", component.m_ChunkManager->GetTotalChunks());
                if (component.m_TessellationEnabled)
                {
                    ImGui::Text("Visible (LOD): %u",
                        static_cast<u32>(component.m_ChunkManager->GetSelectedChunks().size()));
                    ImGui::Text("Quadtree Nodes: %u",
                        component.m_ChunkManager->GetQuadtree().GetNodeCount());
                }
            } });

        DrawComponent<FoliageComponent>("Foliage", entity, [](auto& component)
                                        {
                ImGui::Checkbox("Enabled", &component.m_Enabled);

                ImGui::Separator();
                ImGui::Text("Layers: %u", static_cast<u32>(component.m_Layers.size()));

                if (ImGui::Button("+ Add Layer"))
                {
                    FoliageLayer newLayer;
                    newLayer.Name = "Layer " + std::to_string(component.m_Layers.size());
                    component.m_Layers.push_back(newLayer);
                    component.m_NeedsRebuild = true;
                }

                i32 removeIdx = -1;
                for (sizet i = 0; i < component.m_Layers.size(); ++i)
                {
                    auto& layer = component.m_Layers[i];
                    ImGui::PushID(static_cast<int>(i));

                    bool layerOpen = ImGui::TreeNodeEx(layer.Name.c_str(),
                        ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_Framed);

                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
                    if (ImGui::SmallButton("Remove"))
                    {
                        removeIdx = static_cast<i32>(i);
                    }

                    if (layerOpen)
                    {
                        char nameBuf[128];
                        std::strncpy(nameBuf, layer.Name.c_str(), sizeof(nameBuf) - 1);
                        nameBuf[sizeof(nameBuf) - 1] = '\0';
                        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
                        {
                            layer.Name = nameBuf;
                        }
                        ImGui::Checkbox("Layer Enabled", &layer.Enabled);

                        // Paths
                        char meshBuf[256];
                        std::strncpy(meshBuf, layer.MeshPath.c_str(), sizeof(meshBuf) - 1);
                        meshBuf[sizeof(meshBuf) - 1] = '\0';
                        if (ImGui::InputText("Mesh Path", meshBuf, sizeof(meshBuf)))
                        {
                            layer.MeshPath = meshBuf;
                            component.m_NeedsRebuild = true;
                        }

                        char albedoBuf[256];
                        std::strncpy(albedoBuf, layer.AlbedoPath.c_str(), sizeof(albedoBuf) - 1);
                        albedoBuf[sizeof(albedoBuf) - 1] = '\0';
                        if (ImGui::InputText("Albedo Path", albedoBuf, sizeof(albedoBuf)))
                        {
                            layer.AlbedoPath = albedoBuf;
                            component.m_NeedsRebuild = true;
                        }

                        ImGui::Separator();
                        ImGui::Text("Placement");

                        if (ImGui::DragFloat("Density", &layer.Density, 0.01f, 0.001f, 100.0f))
                            component.m_NeedsRebuild = true;
                        if (ImGui::DragInt("Splatmap Channel", &layer.SplatmapChannel, 1, -1, 7))
                            component.m_NeedsRebuild = true;
                        if (ImGui::DragFloat("Min Slope", &layer.MinSlopeAngle, 0.5f, 0.0f, 90.0f))
                            component.m_NeedsRebuild = true;
                        if (ImGui::DragFloat("Max Slope", &layer.MaxSlopeAngle, 0.5f, 0.0f, 90.0f))
                            component.m_NeedsRebuild = true;

                        ImGui::Separator();
                        ImGui::Text("Scale & Height");

                        if (ImGui::DragFloat("Min Scale", &layer.MinScale, 0.01f, 0.01f, 10.0f))
                            component.m_NeedsRebuild = true;
                        if (ImGui::DragFloat("Max Scale", &layer.MaxScale, 0.01f, 0.01f, 10.0f))
                            component.m_NeedsRebuild = true;
                        if (ImGui::DragFloat("Min Height", &layer.MinHeight, 0.01f, 0.01f, 10.0f))
                            component.m_NeedsRebuild = true;
                        if (ImGui::DragFloat("Max Height", &layer.MaxHeight, 0.01f, 0.01f, 10.0f))
                            component.m_NeedsRebuild = true;
                        if (ImGui::Checkbox("Random Rotation", &layer.RandomRotation))
                            component.m_NeedsRebuild = true;

                        ImGui::Separator();
                        ImGui::Text("Rendering");

                        ImGui::DragFloat("View Distance", &layer.ViewDistance, 1.0f, 10.0f, 1000.0f);
                        ImGui::DragFloat("Fade Start", &layer.FadeStartDistance, 1.0f, 5.0f, 1000.0f);

                        ImGui::Separator();
                        ImGui::Text("Wind");

                        ImGui::DragFloat("Wind Strength", &layer.WindStrength, 0.01f, 0.0f, 5.0f);
                        ImGui::DragFloat("Wind Speed", &layer.WindSpeed, 0.1f, 0.0f, 20.0f);

                        ImGui::Separator();
                        ImGui::Text("Material");

                        ImGui::ColorEdit3("Base Color", glm::value_ptr(layer.BaseColor));
                        ImGui::DragFloat("Roughness", &layer.Roughness, 0.01f, 0.0f, 1.0f);
                        ImGui::DragFloat("Alpha Cutoff", &layer.AlphaCutoff, 0.01f, 0.0f, 1.0f);

                        ImGui::TreePop();
                    }

                    ImGui::PopID();
                }

                if (removeIdx >= 0)
                {
                    component.m_Layers.erase(component.m_Layers.begin() + removeIdx);
                    component.m_NeedsRebuild = true;
                }

                ImGui::Separator();
                if (ImGui::Button("Rebuild Foliage"))
                {
                    component.m_NeedsRebuild = true;
                }

                // Runtime info
                if (component.m_Renderer)
                {
                    ImGui::Text("Total Instances: %u", component.m_Renderer->GetTotalInstanceCount());
                    ImGui::Text("Visible Instances: %u", component.m_Renderer->GetVisibleInstanceCount());
                } });

        DrawComponent<WaterComponent>("Water", entity, [](auto& component)
                                      {
                ImGui::Checkbox("Enabled", &component.m_Enabled);

                ImGui::SeparatorText("Geometry");
                if (ImGui::DragFloat("World Size X", &component.m_WorldSizeX, 1.0f, 0.1f, 100000.0f))
                    component.m_NeedsRebuild = true;
                if (ImGui::DragFloat("World Size Z", &component.m_WorldSizeZ, 1.0f, 0.1f, 100000.0f))
                    component.m_NeedsRebuild = true;

                if (i32 resX = static_cast<i32>(component.m_GridResolutionX); ImGui::DragInt("Grid Resolution X", &resX, 1, 2, 512))
                {
                    component.m_GridResolutionX = static_cast<u32>(resX);
                    component.m_NeedsRebuild = true;
                }
                if (i32 resZ = static_cast<i32>(component.m_GridResolutionZ); ImGui::DragInt("Grid Resolution Z", &resZ, 1, 2, 512))
                {
                    component.m_GridResolutionZ = static_cast<u32>(resZ);
                    component.m_NeedsRebuild = true;
                }

                ImGui::SeparatorText("Waves");
                ImGui::DragFloat("Amplitude", &component.m_WaveAmplitude, 0.01f, 0.0f, 100.0f);
                ImGui::DragFloat("Frequency", &component.m_WaveFrequency, 0.01f, 0.0f, 100.0f);
                ImGui::DragFloat("Speed", &component.m_WaveSpeed, 0.01f, 0.0f, 100.0f);
                if (ImGui::DragFloat2("Direction 0", glm::value_ptr(component.m_WaveDir0), 0.01f, -1.0f, 1.0f))
                {
                    if (f32 len = glm::length(component.m_WaveDir0); len > 1e-6f)
                        component.m_WaveDir0 = glm::normalize(component.m_WaveDir0);
                    else
                        component.m_WaveDir0 = { 1.0f, 0.0f };
                }
                ImGui::DragFloat("Steepness 0", &component.m_WaveSteepness0, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Wavelength 0", &component.m_Wavelength0, 0.1f, 0.1f, 500.0f);
                if (ImGui::DragFloat2("Direction 1", glm::value_ptr(component.m_WaveDir1), 0.01f, -1.0f, 1.0f))
                {
                    if (f32 len = glm::length(component.m_WaveDir1); len > 1e-6f)
                        component.m_WaveDir1 = glm::normalize(component.m_WaveDir1);
                    else
                        component.m_WaveDir1 = { 0.7071f, 0.7071f };
                }
                ImGui::DragFloat("Steepness 1", &component.m_WaveSteepness1, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Wavelength 1", &component.m_Wavelength1, 0.1f, 0.1f, 500.0f);

                ImGui::SeparatorText("Appearance");
                ImGui::ColorEdit3("Shallow Color", glm::value_ptr(component.m_WaterColor));
                ImGui::ColorEdit3("Deep Color", glm::value_ptr(component.m_DeepColor));
                ImGui::DragFloat("Transparency", &component.m_Transparency, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Reflectivity", &component.m_Reflectivity, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Fresnel Power", &component.m_FresnelPower, 0.1f, 0.1f, 20.0f);
                ImGui::DragFloat("Specular Intensity", &component.m_SpecularIntensity, 0.01f, 0.0f, 10.0f);

                ImGui::SeparatorText("Normal Maps");
                ImGui::DragFloat("Normal Map Tiling", &component.m_NormalMapTiling, 0.01f, 0.0f, 50.0f);
                if (ImGui::DragFloat2("Scroll Dir 0", glm::value_ptr(component.m_NormalMapScrollDir0), 0.01f, -1.0f, 1.0f))
                {
                    if (auto const len = glm::length(component.m_NormalMapScrollDir0); len > 1e-4f)
                        component.m_NormalMapScrollDir0 /= len;
                    else
                        component.m_NormalMapScrollDir0 = { 1.0f, 0.0f };
                }
                ImGui::DragFloat("Scroll Speed 0", &component.m_NormalMapScrollSpeed0, 0.001f, 0.0f, 1.0f);
                if (ImGui::DragFloat2("Scroll Dir 1", glm::value_ptr(component.m_NormalMapScrollDir1), 0.01f, -1.0f, 1.0f))
                {
                    if (auto const len = glm::length(component.m_NormalMapScrollDir1); len > 1e-4f)
                        component.m_NormalMapScrollDir1 /= len;
                    else
                        component.m_NormalMapScrollDir1 = { 0.0f, 1.0f };
                }
                ImGui::DragFloat("Scroll Speed 1", &component.m_NormalMapScrollSpeed1, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Noise Intensity", &component.m_NoiseIntensity, 0.01f, 0.0f, 1.0f);

                // Normal Map 0 drag-drop
                {
                    ImGui::Text("Normal Map 0: %s", component.m_NormalMap0 != 0 ? "Set" : "None");
                    ImGui::SameLine();
                    ImGui::Button("Drop##NormalMap0", ImVec2(60.0f, 0.0f));
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            std::filesystem::path texPath = PathFromUtf8Payload(*payload);
                            if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                            {
                                if (auto const imported = assetManager->ImportAsset(texPath);
                                    imported != 0 && AssetManager::GetAssetType(imported) == AssetType::Texture2D)
                                    component.m_NormalMap0 = imported;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (component.m_NormalMap0 != 0)
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X##ClearNM0"))
                            component.m_NormalMap0 = 0;
                    }
                }

                // Normal Map 1 drag-drop
                {
                    ImGui::Text("Normal Map 1: %s", component.m_NormalMap1 != 0 ? "Set" : "None");
                    ImGui::SameLine();
                    ImGui::Button("Drop##NormalMap1", ImVec2(60.0f, 0.0f));
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            std::filesystem::path texPath = PathFromUtf8Payload(*payload);
                            if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                            {
                                if (auto const imported = assetManager->ImportAsset(texPath);
                                    imported != 0 && AssetManager::GetAssetType(imported) == AssetType::Texture2D)
                                    component.m_NormalMap1 = imported;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (component.m_NormalMap1 != 0)
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X##ClearNM1"))
                            component.m_NormalMap1 = 0;
                    }
                }

                // Noise Texture drag-drop
                {
                    ImGui::Text("Noise Texture: %s", component.m_NoiseTexture != 0 ? "Set" : "None");
                    ImGui::SameLine();
                    ImGui::Button("Drop##NoiseTex", ImVec2(60.0f, 0.0f));
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            std::filesystem::path texPath = PathFromUtf8Payload(*payload);
                            if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                            {
                                if (auto const imported = assetManager->ImportAsset(texPath);
                                    imported != 0 && AssetManager::GetAssetType(imported) == AssetType::Texture2D)
                                    component.m_NoiseTexture = imported;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (component.m_NoiseTexture != 0)
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X##ClearNoise"))
                            component.m_NoiseTexture = 0;
                    }
                }

                ImGui::SeparatorText("Depth & Refraction");
                ImGui::Checkbox("Refraction Enabled", &component.m_RefractionEnabled);
                ImGui::DragFloat("Depth Softening", &component.m_DepthSofteningDistance, 0.1f, 0.0f, 50.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Distance over which water edges fade (shoreline softening)");
                ImGui::DragFloat("Refraction Distortion", &component.m_RefractionDistortion, 0.001f, 0.0f, 0.5f, "%.3f");
                ImGui::DragFloat("Refraction Height Factor", &component.m_RefractionHeightFactor, 0.01f, 0.0f, 2.0f);
                ImGui::ColorEdit3("Refraction Tint", glm::value_ptr(component.m_RefractionColor));

                ImGui::SeparatorText("Foam");
                // Foam texture drag-drop
                {
                    ImGui::Text("Foam Texture: %s", component.m_FoamTexture != 0 ? "Set" : "None");
                    ImGui::SameLine();
                    ImGui::Button("Drop##FoamTex", ImVec2(60.0f, 0.0f));
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            std::filesystem::path texPath = PathFromUtf8Payload(*payload);
                            if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                            {
                                if (auto const imported = assetManager->ImportAsset(texPath);
                                    imported != 0 && AssetManager::GetAssetType(imported) == AssetType::Texture2D)
                                    component.m_FoamTexture = imported;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (component.m_FoamTexture != 0)
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("X##ClearFoam"))
                            component.m_FoamTexture = 0;
                    }
                }
                ImGui::DragFloat("Foam Height Start", &component.m_FoamHeightStart, 0.01f, 0.0f, 2.0f);
                ImGui::DragFloat("Foam Fade Distance", &component.m_FoamFadeDistance, 0.01f, 0.01f, 5.0f);
                ImGui::DragFloat("Foam Tiling", &component.m_FoamTiling, 0.1f, 0.0f, 50.0f);
                ImGui::DragFloat("Foam Brightness", &component.m_FoamBrightness, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Foam Angle Exponent", &component.m_FoamAngleExponent, 0.1f, 0.1f, 10.0f);
                ImGui::DragFloat("Shoreline Foam Power", &component.m_ShorelineFoamPower, 0.1f, 0.1f, 10.0f);

                ImGui::SeparatorText("Subsurface Scattering");
                ImGui::ColorEdit3("SSS Color", glm::value_ptr(component.m_SSSColor));
                ImGui::DragFloat("SSS Intensity", &component.m_SSSIntensity, 0.01f, 0.0f, 5.0f);

                ImGui::SeparatorText("Screen Space Reflections");
                ImGui::Checkbox("SSR Enabled", &component.m_SSREnabled);
                ImGui::DragFloat("SSR Max Steps", &component.m_SSRMaxSteps, 1.0f, 0.0f, 256.0f, "%.0f");
                ImGui::DragFloat("SSR Step Size", &component.m_SSRStepSize, 0.01f, 0.01f, 1.0f);
                ImGui::DragFloat("SSR Max Distance", &component.m_SSRMaxDistance, 1.0f, 1.0f, 200.0f);
                ImGui::DragFloat("SSR Thickness", &component.m_SSRThickness, 0.05f, 0.01f, 5.0f);

                ImGui::SeparatorText("Tessellation");
                ImGui::Checkbox("Tessellation Enabled", &component.m_TessellationEnabled);
                if (component.m_TessellationEnabled)
                {
                    ImGui::DragFloat("Tessellation Factor", &component.m_TessellationFactor, 0.5f, 1.0f, 64.0f, "%.1f");
                    if (ImGui::DragFloat("Tess Min Distance", &component.m_TessMinDistance, 1.0f, 1.0f, 500.0f))
                        component.m_TessMaxDistance = std::max(component.m_TessMaxDistance, component.m_TessMinDistance + 1.0f);
                    if (ImGui::DragFloat("Tess Max Distance", &component.m_TessMaxDistance, 1.0f, 10.0f, 1000.0f))
                        component.m_TessMaxDistance = std::max(component.m_TessMaxDistance, component.m_TessMinDistance + 1.0f);
                }

                ImGui::Separator();
                if (ImGui::Button("Rebuild Mesh"))
                {
                    component.m_NeedsRebuild = true;
                } });

        DrawComponent<SnowDeformerComponent>("Snow Deformer", entity, [](auto& component)
                                             {
                ImGui::DragFloat("Deform Radius", &component.m_DeformRadius, 0.01f, 0.01f, 10.0f, "%.2f");
                ImGui::DragFloat("Deform Depth", &component.m_DeformDepth, 0.01f, 0.0f, 2.0f, "%.3f");
                ImGui::DragFloat("Falloff Exponent", &component.m_FalloffExponent, 0.1f, 0.1f, 10.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("1 = linear, 2 = quadratic, higher = sharper edge");
                ImGui::DragFloat("Compaction Factor", &component.m_CompactionFactor, 0.01f, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = full removal, 1 = compact only");
                ImGui::Checkbox("Emit Ejecta", &component.m_EmitEjecta); });

        DrawComponent<FogVolumeComponent>("Fog Volume", entity, [](auto& component)
                                          {
                ImGui::Checkbox("Enabled##FogVolume", &component.m_Enabled);

                const char* shapeNames[] = { "Box", "Sphere", "Cylinder" };
                if (int shape = static_cast<int>(component.m_Shape); ImGui::Combo("Shape##FogVolume", &shape, shapeNames, IM_ARRAYSIZE(shapeNames)))
                    component.m_Shape = static_cast<FogVolumeShape>(shape);

                if (component.m_Shape == FogVolumeShape::Sphere)
                {
                    ImGui::DragFloat("Radius##FogVolume", &component.m_Extents.x, 0.1f, 0.1f, 500.0f, "%.1f");
                }
                else if (component.m_Shape == FogVolumeShape::Cylinder)
                {
                    ImGui::DragFloat("Radius##FogVolCyl", &component.m_Extents.x, 0.1f, 0.1f, 500.0f, "%.1f");
                    ImGui::DragFloat("Half Height##FogVolCyl", &component.m_Extents.y, 0.1f, 0.1f, 500.0f, "%.1f");
                }
                else
                {
                    DrawVec3Control("Half Extents", component.m_Extents);
                }

                ImGui::ColorEdit3("Color##FogVolume", glm::value_ptr(component.m_Color));
                ImGui::DragFloat("Density##FogVolume", &component.m_Density, 0.01f, 0.0f, 100.0f, "%.3f");
                ImGui::DragFloat("Falloff Distance##FogVolume", &component.m_FalloffDistance, 0.05f, 0.0f, 100.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Boundary fade distance in world-space units");
                ImGui::DragInt("Priority##FogVolume", &component.m_Priority, 1, -100, 100);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Higher priority volumes blend on top");
                ImGui::DragFloat("Blend Weight##FogVolume", &component.m_BlendWeight, 0.01f, 0.0f, 1.0f, "%.2f");
                ImGui::Checkbox("Affect Transparent##FogVolume", &component.m_AffectTransparent); });

        DrawComponent<DecalComponent>("Decal", entity, [](auto& component)
                                      {
                ImGui::ColorEdit4("Color##Decal", glm::value_ptr(component.m_Color));
                DrawVec3Control("Size", component.m_Size);
                constexpr f32 kMinDecalAxis = 1e-3f;
                component.m_Size.x = glm::max(component.m_Size.x, kMinDecalAxis);
                component.m_Size.y = glm::max(component.m_Size.y, kMinDecalAxis);
                component.m_Size.z = glm::max(component.m_Size.z, kMinDecalAxis);
                ImGui::DragFloat("Fade Distance##Decal", &component.m_FadeDistance, 0.01f, 0.0f, 10.0f, "%.2f");
                ImGui::SliderFloat("Normal Threshold##Decal", &component.m_NormalAngleThreshold, 0.0f, 1.0f, "%.2f");

                // Deferred G-Buffer target channel.
                static const char* kDecalModes[] = { "Albedo", "Normal", "RMA", "Emissive" };
                if (i32 currentMode = static_cast<i32>(component.m_Mode); ImGui::Combo("Mode##Decal", &currentMode, kDecalModes, IM_ARRAYSIZE(kDecalModes)))
                {
                    component.m_Mode = static_cast<DecalMode>(currentMode);
                }

                ImGui::Checkbox("Transparent##Decal", &component.m_Transparent);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Route this decal through the forward (alpha-blended /\n"
                                      "WB-OIT) pipeline instead of the deferred G-Buffer\n"
                                      "overlay. Required for decals that need to blend\n"
                                      "against the lit scene colour (glass stickers, smoke\n"
                                      "puddles). Ignored in Forward/Forward+ paths — all\n"
                                      "forward decals are already transparent overlays.");
                }

                // srgb selects the GPU colour-space conversion: albedo and
                // emissive are authored colour (true), normal/RMA are linear
                // data (false).
                auto drawTextureSlot = [](const char* label, Ref<Texture2D>& slot, bool srgb)
                {
                    ImGui::PushID(label);
                    ImGui::Button(label, ImVec2(100.0f, 0.0f));
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                        {
                            std::filesystem::path texturePath = PathFromUtf8Payload(*payload);
                            Ref<Texture2D> const texture = Texture2D::Create(texturePath.string(), srgb);
                            if (texture && texture->IsLoaded())
                            {
                                slot = texture;
                            }
                            else
                            {
                                OLO_WARN("Could not load texture {0}", texturePath.filename().string());
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!slot);
                    if (ImGui::Button("X", ImVec2(20.0f, 0.0f)))
                        slot = nullptr;
                    ImGui::EndDisabled();
                    if (slot && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Clear texture slot");
                    ImGui::PopID();
                };

                drawTextureSlot("Albedo Texture", component.m_AlbedoTexture, /*srgb=*/true);
                drawTextureSlot("Normal Texture", component.m_NormalTexture, /*srgb=*/false);
                drawTextureSlot("RMA Texture", component.m_RMATexture, /*srgb=*/false);
                drawTextureSlot("Emissive Texture", component.m_EmissiveTexture, /*srgb=*/true); });

        DrawComponent<LightProbeComponent>("Light Probe", entity, [](auto& component)
                                           {
                ImGui::DragFloat("Influence Radius", &component.m_InfluenceRadius, 0.1f, 0.1f, 100.0f, "%.1f");
                ImGui::DragFloat("Intensity##LightProbe", &component.m_Intensity, 0.01f, 0.0f, 10.0f, "%.2f");
                ImGui::Checkbox("Active##LightProbe", &component.m_Active); });

        DrawComponent<LightProbeVolumeComponent>("Light Probe Volume", entity, [this](auto& component)
                                                 {
                {
                    glm::vec3 const prevMin = component.m_BoundsMin;
                    glm::vec3 const prevMax = component.m_BoundsMax;
                    DrawVec3Control("Bounds Min", component.m_BoundsMin);
                    DrawVec3Control("Bounds Max", component.m_BoundsMax);
                    if (component.m_BoundsMin != prevMin || component.m_BoundsMax != prevMax)
                    {
                        component.m_Dirty = true;
                    }
                }
                if (i32 res[3] = { component.m_Resolution.x, component.m_Resolution.y, component.m_Resolution.z }; ImGui::DragInt3("Resolution", res, 1, 1, 64))
                {
                    component.m_Resolution = glm::ivec3(res[0], res[1], res[2]);
                    component.m_Dirty = true;
                }
                if (ImGui::DragFloat("Spacing", &component.m_Spacing, 0.1f, 0.1f, 50.0f, "%.1f"))
                {
                    component.m_Dirty = true;
                }
                if (ImGui::DragFloat("Intensity##ProbeVolume", &component.m_Intensity, 0.01f, 0.0f, 10.0f, "%.2f"))
                {
                    component.m_Dirty = true;
                }
                if (ImGui::Checkbox("Active##ProbeVolume", &component.m_Active))
                {
                    component.m_Dirty = true;
                }
                ImGui::Text("Total Probes: %d", component.GetTotalProbeCount());
                ImGui::Text("Baked Data: %s", component.m_BakedDataAsset != 0 ? "Yes" : "No");
                ImGui::Checkbox("Show Debug Probes", &component.m_ShowDebugProbes);

                ImGui::Separator();

                // Bake progress state (static per-frame tracking)
                static f32 s_BakeProgress = 0.0f;
                static bool s_BakeInProgress = false;

                if (s_BakeInProgress)
                {
                    ImGui::ProgressBar(s_BakeProgress, ImVec2(-1.0f, 0.0f), "Baking...");
                }

                if (ImGui::Button("Bake Light Probes"))
                {
                    OLO_PROFILE_SCOPE("Bake Light Probes");
                    s_BakeInProgress = true;
                    s_BakeProgress = 0.0f;
                    auto asset = Ref<LightProbeVolumeAsset>::Create();
                    LightProbeBaker::BakeVolume(
                        m_Context,
                        component,
                        asset,
                        64,
                        [](u32 current, u32 total)
                        {
                            s_BakeProgress = static_cast<f32>(current) / static_cast<f32>(total);
                            OLO_CORE_INFO("Baking probe {}/{}", current, total);
                        });
                    s_BakeInProgress = false;
                    s_BakeProgress = 1.0f;

                    // Register the baked asset and assign its handle to the component
                    AssetHandle handle = AssetManager::AddMemoryOnlyAsset(asset);
                    component.m_BakedDataAsset = handle;
                    component.m_Dirty = true;
                    OLO_CORE_INFO("Light probe baking complete ({} probes), asset handle: {}", component.GetTotalProbeCount(), handle);
                }
                if (s_BakeProgress > 0.0f && !s_BakeInProgress)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Complete!");
                } });

        DrawComponent<ReflectionProbeComponent>("Reflection Probe", entity, [this, entity](auto& component)
                                                {
            if (ImGui::DragFloat("Influence Radius", &component.m_InfluenceRadius, 0.1f, 0.1f, 1000.0f, "%.2f"))
            {
                component.m_NeedsBake = true;
            }
            ImGui::DragFloat("Blend Distance", &component.m_BlendDistance, 0.05f, 0.0f, 100.0f, "%.2f");
            ImGui::DragFloat("Intensity##ReflectionProbe", &component.m_Intensity, 0.01f, 0.0f, 10.0f, "%.2f");
            if (i32 resolution = static_cast<i32>(component.m_Resolution); ImGui::SliderInt("Resolution##ReflectionProbe", &resolution, 16, 1024))
            {
                component.m_Resolution = static_cast<u32>(std::clamp(resolution, 16, 2048));
                component.m_NeedsBake = true;
            }
            ImGui::Checkbox("Active##ReflectionProbe", &component.m_Active);

            ImGui::Separator();

            bool const hasBake = component.m_BakedEnvironment && component.m_BakedEnvironment->HasIBL();
            ImGui::Text("Baked: %s", hasBake ? "Yes" : "No");
            if (component.m_NeedsBake && hasBake)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "(stale)");
            }

            if (ImGui::Button("Bake Reflection Probe"))
            {
                OLO_PROFILE_SCOPE("Bake Reflection Probe");
                glm::vec3 position(0.0f);
                if (entity.template HasComponent<TransformComponent>())
                {
                    position = entity.template GetComponent<TransformComponent>().Translation;
                }
                bool const ok = ReflectionProbeBaker::BakeProbe(m_Context, position, component);
                if (ok)
                {
                    OLO_CORE_INFO("Reflection probe baked at ({}, {}, {})", position.x, position.y, position.z);
                }
                else
                {
                    OLO_CORE_ERROR("Reflection probe bake failed");
                }
            } });

        DrawComponent<StreamingVolumeComponent>("Streaming Volume", entity, [](auto& component)
                                                {
            // Region assignment: show current handle and browse button
            ImGui::Text("Region ID: %llu", static_cast<unsigned long long>(static_cast<u64>(component.RegionAssetHandle)));

            if (ImGui::Button("Browse Region File..."))
            {
                std::string filepath = FileDialogs::OpenFile(
                    "Streaming Region (*.oloregion)\0*.oloregion\0");
                if (!filepath.empty())
                {
                    // Parse the .oloregion header to extract RegionID
                    auto data = StreamingRegionSerializer::ParseRegionFile(filepath);
                    if (data && data["RegionID"])
                    {
                        component.RegionAssetHandle = data["RegionID"].as<u64>(0);
                    }
                }
            }

            // Drag-drop from ContentBrowser
            if (ImGui::BeginDragDropTarget())
            {
                if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_REGION"))
                {
                    std::filesystem::path regionPath = PathFromUtf8Payload(*payload);
                    auto data = StreamingRegionSerializer::ParseRegionFile(regionPath);
                    if (data && data["RegionID"])
                    {
                        component.RegionAssetHandle = data["RegionID"].as<u64>(0);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Activation mode combo
            const char* modeNames[] = { "Proximity", "Manual" };
            if (int currentMode = static_cast<int>(component.ActivationMode); ImGui::Combo("Activation Mode", &currentMode, modeNames, IM_ARRAYSIZE(modeNames)))
            {
                component.ActivationMode = static_cast<StreamingActivationMode>(currentMode);
            }

            ImGui::DragFloat("Load Radius", &component.LoadRadius, 1.0f, 10.0f, 10000.0f);
            ImGui::DragFloat("Unload Radius", &component.UnloadRadius, 1.0f, 10.0f, 10000.0f);

            if (component.UnloadRadius < component.LoadRadius)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f),
                    "Warning: Unload < Load radius causes thrashing");
            }

            ImGui::Text("Loaded: %s", component.IsLoaded ? "Yes" : "No"); });

        DrawComponent<NetworkIdentityComponent>("Network Identity", entity, [](auto& component)
                                                {
            ImGui::DragScalar("Owner Client ID", ImGuiDataType_U32, &component.OwnerClientID);

            const char* authorityStrings[] = { "Server", "Client", "Shared" };
            if (int currentAuthority = static_cast<int>(component.Authority); ImGui::Combo("Authority", &currentAuthority, authorityStrings, IM_ARRAYSIZE(authorityStrings)))
            {
                component.Authority = static_cast<ENetworkAuthority>(currentAuthority);
            }

            ImGui::Checkbox("Is Replicated", &component.IsReplicated); });

        DrawComponent<DialogueComponent>("Dialogue", entity, [](auto& component)
                                         {
            // Asset handle display
            if (component.m_DialogueTree != 0)
            {
                auto metadata = AssetManager::GetAssetMetadata(component.m_DialogueTree);
                if (metadata.IsValid())
                    ImGui::Text("Asset: %s", metadata.FilePath.filename().string().c_str());
                else
                    ImGui::Text("Asset: <invalid handle>");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No dialogue assigned");
            }

            // Browse button
            if (ImGui::Button("Browse...##DialogueAsset"))
            {
                std::string filepath = FileDialogs::OpenFile(
                    "Dialogue Tree (*.olodialogue)\0*.olodialogue\0"
                    "All Files (*.*)\0*.*\0");
                if (!filepath.empty())
                {
                    auto assetManager = Project::GetAssetManager().As<EditorAssetManager>();
                    if (assetManager)
                    {
                        AssetHandle importedHandle = assetManager->ImportAsset(filepath);
                        if (importedHandle != 0)
                        {
                            auto metadata = AssetManager::GetAssetMetadata(importedHandle);
                            if (metadata.Type == AssetType::DialogueTree)
                                component.m_DialogueTree = importedHandle;
                            else
                                OLO_CORE_WARN("Imported asset is not a DialogueTree");
                        }
                    }
                }
            }

            if (component.m_DialogueTree != 0)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear"))
                    component.m_DialogueTree = 0;
            }

            // Drag-drop target (also accepts drops on the Browse button)
            if (ImGui::BeginDragDropTarget())
            {
                if (ImGuiPayload const* const payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    std::filesystem::path assetPath = PathFromUtf8Payload(*payload);
                    if (assetPath.extension() == ".olodialogue")
                    {
                        auto assetManager = Project::GetAssetManager().As<EditorAssetManager>();
                        if (assetManager)
                        {
                            AssetHandle importedHandle = assetManager->ImportAsset(assetPath);
                            if (importedHandle != 0)
                            {
                                auto metadata = AssetManager::GetAssetMetadata(importedHandle);
                                if (metadata.Type == AssetType::DialogueTree)
                                    component.m_DialogueTree = importedHandle;
                                else
                                    OLO_CORE_WARN("Dropped asset is not a DialogueTree");
                            }
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::Separator();
            ImGui::Checkbox("Auto Trigger", &component.m_AutoTrigger);
            ImGui::DragFloat("Trigger Radius", &component.m_TriggerRadius, 0.1f, 0.0f, 100.0f);
            ImGui::Checkbox("Trigger Once", &component.m_TriggerOnce);

            if (component.m_HasTriggered)
            {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Already triggered)");
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset"))
                    component.m_HasTriggered = false;
            } });

        DrawComponent<NavMeshBoundsComponent>("NavMesh Bounds", entity, [](auto& component)
                                              {
            ImGui::DragFloat3("Min", &component.m_Min.x, 1.0f);
            ImGui::DragFloat3("Max", &component.m_Max.x, 1.0f);
            for (int i = 0; i < 3; ++i)
            {
                if (component.m_Min[i] > component.m_Max[i])
                    std::swap(component.m_Min[i], component.m_Max[i]);
            } });

        DrawComponent<NavAgentComponent>("Nav Agent", entity, [](auto& component)
                                         {
            ImGui::DragFloat("Radius", &component.m_Radius, 0.01f, 0.01f, 100.0f);
            ImGui::DragFloat("Height", &component.m_Height, 0.01f, 0.01f, 100.0f);
            ImGui::DragFloat("Max Speed", &component.m_MaxSpeed, 0.1f, 0.0f, 1000.0f);
            ImGui::DragFloat("Acceleration", &component.m_Acceleration, 0.1f, 0.0f, 1000.0f);
            ImGui::DragFloat("Stopping Distance", &component.m_StoppingDistance, 0.01f, 0.0f, 100.0f);
            ImGui::DragInt("Avoidance Priority", &component.m_AvoidancePriority, 1, 0, 100);
            ImGui::Checkbox("Lock Y Axis", &component.m_LockYAxis);

            if (component.m_HasPath)
            {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Has path (%d corners)", static_cast<int>(component.m_PathCorners.size()));
            }
            else
            {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No active path");
            } });

        DrawComponent<BehaviorTreeComponent>("Behavior Tree", entity, [](auto& component)
                                             {
            if (component.BehaviorTreeAssetHandle != 0)
            {
                auto metadata = AssetManager::GetAssetMetadata(component.BehaviorTreeAssetHandle);
                if (metadata.IsValid())
                    ImGui::Text("Asset: %s", metadata.FilePath.filename().string().c_str());
                else
                    ImGui::Text("Asset: <invalid handle>");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No behavior tree assigned");
            }

            if (ImGui::Button("Browse...##BTAsset"))
            {
                std::string filepath = FileDialogs::OpenFile(
                    "Behavior Tree (*.olobt)\0*.olobt\0"
                    "All Files (*.*)\0*.*\0");
                if (!filepath.empty())
                {
                    auto assetManager = Project::GetAssetManager().As<EditorAssetManager>();
                    if (assetManager)
                    {
                        AssetHandle importedHandle = assetManager->ImportAsset(filepath);
                        if (importedHandle != 0)
                        {
                            auto metadata = AssetManager::GetAssetMetadata(importedHandle);
                            if (metadata.Type == AssetType::BehaviorTree)
                                component.BehaviorTreeAssetHandle = importedHandle;
                        }
                    }
                }
            }

            if (component.BehaviorTreeAssetHandle != 0)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##BT"))
                    component.BehaviorTreeAssetHandle = 0;
            }

            if (component.IsRunning)
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Running");
            else
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Not running"); });

        DrawComponent<StateMachineComponent>("State Machine", entity, [](auto& component)
                                             {
            if (component.StateMachineAssetHandle != 0)
            {
                auto metadata = AssetManager::GetAssetMetadata(component.StateMachineAssetHandle);
                if (metadata.IsValid())
                    ImGui::Text("Asset: %s", metadata.FilePath.filename().string().c_str());
                else
                    ImGui::Text("Asset: <invalid handle>");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No state machine assigned");
            }

            if (ImGui::Button("Browse...##FSMAsset"))
            {
                std::string filepath = FileDialogs::OpenFile(
                    "State Machine (*.olofsm)\0*.olofsm\0"
                    "All Files (*.*)\0*.*\0");
                if (!filepath.empty())
                {
                    auto assetManager = Project::GetAssetManager().As<EditorAssetManager>();
                    if (assetManager)
                    {
                        AssetHandle importedHandle = assetManager->ImportAsset(filepath);
                        if (importedHandle != 0)
                        {
                            auto metadata = AssetManager::GetAssetMetadata(importedHandle);
                            if (metadata.Type == AssetType::StateMachine)
                                component.StateMachineAssetHandle = importedHandle;
                        }
                    }
                }
            }

            if (component.StateMachineAssetHandle != 0)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##FSM"))
                    component.StateMachineAssetHandle = 0;
            }

            if (component.RuntimeFSM && component.RuntimeFSM->IsStarted())
            {
                ImGui::Text("Current State: %s", component.RuntimeFSM->GetCurrentStateID().c_str());
            }
            else
            {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Not started");
            } });

        DrawComponent<InventoryComponent>("Inventory", entity, [](auto& component)
                                          {
            if (i32 capacity = component.PlayerInventory.GetCapacity(); ImGui::DragInt("Capacity", &capacity, 1, 1, 1000))
                component.PlayerInventory.SetCapacity(capacity);

            ImGui::DragFloat("Max Weight", &component.PlayerInventory.MaxWeight, 0.1f, 0.0f, 10000.0f);
            ImGui::DragInt("Currency", &component.Currency, 1, 0, 999999);

            ImGui::Separator();
            ImGui::Text("Used Slots: %d / %d", component.PlayerInventory.GetUsedSlots(), component.PlayerInventory.GetCapacity());
            ImGui::Text("Total Weight: %.1f", component.PlayerInventory.GetTotalWeight());

            // Show inventory grid
            if (ImGui::TreeNode("Inventory Slots"))
            {
                for (i32 slot = 0; slot < component.PlayerInventory.GetCapacity(); ++slot)
                {
                    const auto* item = component.PlayerInventory.GetItemAtSlot(slot);
                    if (item)
                    {
                        const auto* def = ItemDatabase::Get(item->ItemDefinitionID);
                        std::string label = def ? def->DisplayName : item->ItemDefinitionID;
                        ImGui::Text("[%d] %s x%d", slot, label.c_str(), item->StackCount);

                        if (def && ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::Text("%s", def->Description.c_str());
                            ImGui::Text("Category: %s | Rarity: %s",
                                        ItemCategoryToString(def->Category),
                                        ItemRarityToString(def->Rarity));
                            if (item->Durability >= 0.0f)
                                ImGui::Text("Durability: %.0f / %.0f", item->Durability, item->MaxDurability);
                            ImGui::EndTooltip();
                        }
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 0.5f), "[%d] (empty)", slot);
                    }
                }
                ImGui::TreePop();
            }

            // Show equipment
            if (ImGui::TreeNode("Equipment"))
            {
                for (i32 i = 0; i < EquipmentSlots::SlotCount; ++i)
                {
                    auto eqSlot = static_cast<EquipmentSlots::Slot>(i);
                    const auto* item = component.Equipment.GetEquipped(eqSlot);
                    if (item)
                    {
                        const auto* def = ItemDatabase::Get(item->ItemDefinitionID);
                        std::string label = def ? def->DisplayName : item->ItemDefinitionID;
                        ImGui::Text("%s: %s", EquipmentSlots::SlotToString(eqSlot), label.c_str());
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 0.5f), "%s: (empty)",
                                           EquipmentSlots::SlotToString(eqSlot));
                    }
                }
                ImGui::TreePop();
            }

            // Attribute summary
            if (ImGui::TreeNode("Equipment Bonuses"))
            {
                if (auto modifiers = component.Equipment.GetAllAttributeModifiers(); modifiers.empty())
                {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No bonuses");
                }
                else
                {
                    for (auto const& [attr, val] : modifiers)
                        ImGui::Text("%s: %+.1f", attr.c_str(), val);
                }
                ImGui::TreePop();
            } });

        DrawComponent<ItemPickupComponent>("Item Pickup", entity, [](auto& component)
                                           {
            ImGui::InputText("Item ID", &component.Item.ItemDefinitionID);

            const auto* def = ItemDatabase::Get(component.Item.ItemDefinitionID);
            if (!component.Item.ItemDefinitionID.empty() && !def)
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Unknown item definition");

            i32 maxStack = def ? std::max(def->MaxStackSize, 1) : 9999;
            ImGui::DragInt("Stack Count", &component.Item.StackCount, 1, 1, maxStack);
            component.Item.StackCount = std::clamp(component.Item.StackCount, 1, maxStack);
            ImGui::DragFloat("Pickup Radius", &component.PickupRadius, 0.1f, 0.0f, 100.0f);
            ImGui::Checkbox("Auto Pickup", &component.AutoPickup);
            ImGui::DragFloat("Despawn Timer", &component.DespawnTimer, 0.5f, -1.0f, 600.0f);
            if (component.DespawnTimer < 0.0f)
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(never despawns)"); });

        DrawComponent<ItemContainerComponent>("Item Container", entity, [](auto& component)
                                              {
            if (i32 capacity = component.Contents.GetCapacity(); ImGui::DragInt("Capacity", &capacity, 1, 1, 1000))
                component.Contents.SetCapacity(capacity);

            ImGui::Checkbox("Is Shop", &component.IsShop);

            ImGui::InputText("Loot Table ID", &component.LootTableID);

            ImGui::Separator();
            ImGui::Text("Used: %d / %d", component.Contents.GetUsedSlots(), component.Contents.GetCapacity());

            if (ImGui::TreeNode("Container Items"))
            {
                for (i32 slot = 0; slot < component.Contents.GetCapacity(); ++slot)
                {
                    const auto* item = component.Contents.GetItemAtSlot(slot);
                    if (item)
                    {
                        const auto* def = ItemDatabase::Get(item->ItemDefinitionID);
                        std::string label = def ? def->DisplayName : item->ItemDefinitionID;
                        ImGui::Text("[%d] %s x%d", slot, label.c_str(), item->StackCount);
                    }
                }
                ImGui::TreePop();
            } });

        DrawComponent<QuestJournalComponent>("Quest Journal", entity, [](auto& component)
                                             {
            auto activeQuests = component.Journal.GetActiveQuests();
            auto completedQuests = component.Journal.GetCompletedQuests();

            ImGui::Text("Active Quests: %d", static_cast<int>(activeQuests.size()));
            ImGui::Text("Completed Quests: %d", static_cast<int>(completedQuests.size()));

            if (ImGui::TreeNode("Active Quests"))
            {
                for (auto const& questId : activeQuests)
                {
                    if (ImGui::TreeNode(questId.c_str()))
                    {
                        ImGui::Text("Stage: %d", component.Journal.GetCurrentStageIndex(questId));

                        auto const& states = component.Journal.GetActiveQuestStates();
                        if (auto it = states.find(questId); it != states.end())
                        {
                            for (auto const& obj : it->second.ObjectiveStates)
                            {
                                if (!obj.IsHidden)
                                {
                                    ImVec4 color = obj.IsCompleted ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                                    ImGui::TextColored(color, "%s [%d/%d] %s",
                                        obj.Description.c_str(),
                                        obj.CurrentCount,
                                        obj.RequiredCount,
                                        obj.IsCompleted ? "(done)" : "");
                                }
                            }

                            if (it->second.Definition.TimeLimit > 0.0f)
                            {
                                f32 remaining = it->second.Definition.TimeLimit - it->second.ElapsedTime;
                                ImGui::Text("Time remaining: %.1fs", remaining);
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Completed Quests"))
            {
                for (auto const& questId : completedQuests)
                {
                    ImGui::Text("%s", questId.c_str());
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Tags"))
            {
                for (auto const& tag : component.Journal.GetTags())
                {
                    ImGui::Text("%s", tag.c_str());
                }
                ImGui::TreePop();
            } });

        DrawComponent<QuestGiverComponent>("Quest Giver", entity, [](auto& component)
                                           {
            ImGui::InputText("Marker Icon", &component.QuestMarkerIcon);

            if (ImGui::TreeNode("Offered Quests"))
            {
                for (size_t i = 0; i < component.OfferedQuestIDs.size(); ++i)
                {
                    std::string label = "##offered" + std::to_string(i);
                    ImGui::InputText(label.c_str(), &component.OfferedQuestIDs[i]);
                    ImGui::SameLine();
                    std::string removeLabel = "X##removeOffered" + std::to_string(i);
                    if (ImGui::SmallButton(removeLabel.c_str()))
                    {
                        component.OfferedQuestIDs.erase(component.OfferedQuestIDs.begin() + static_cast<ptrdiff_t>(i));
                        ImGui::TreePop();
                        return;
                    }
                }
                if (ImGui::SmallButton("Add Offered Quest"))
                    component.OfferedQuestIDs.emplace_back();
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Turn-In Quests"))
            {
                for (size_t i = 0; i < component.TurnInQuestIDs.size(); ++i)
                {
                    std::string label = "##turnin" + std::to_string(i);
                    ImGui::InputText(label.c_str(), &component.TurnInQuestIDs[i]);
                    ImGui::SameLine();
                    std::string removeLabel = "X##removeTurnIn" + std::to_string(i);
                    if (ImGui::SmallButton(removeLabel.c_str()))
                    {
                        component.TurnInQuestIDs.erase(component.TurnInQuestIDs.begin() + static_cast<ptrdiff_t>(i));
                        ImGui::TreePop();
                        return;
                    }
                }
                if (ImGui::SmallButton("Add Turn-In Quest"))
                    component.TurnInQuestIDs.emplace_back();
                ImGui::TreePop();
            } });

        DrawComponent<AbilityComponent>("Gameplay Ability", entity, [entity, scene = m_Context](auto& component) mutable
                                        {
            // Attributes section
            if (ImGui::TreeNode("Attributes"))
            {
                auto attrNames = component.Attributes.GetAttributeNames();
                for (auto const& name : attrNames)
                {
                    f32 baseVal = component.Attributes.GetBaseValue(name);
                    f32 currentVal = component.Attributes.GetCurrentValue(name);

                    ImGui::Text("%s", name.c_str());
                    ImGui::SameLine(150.0f);

                    std::string baseLabel = "##base_" + name;
                    ImGui::SetNextItemWidth(80.0f);
                    if (ImGui::DragFloat(baseLabel.c_str(), &baseVal, 0.1f))
                    {
                        component.Attributes.SetBaseValue(name, baseVal);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Base Value");

                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "= %.1f", currentVal);

                    auto const& mods = component.Attributes.GetModifiers(name);
                    if (!mods.empty() && ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        for (auto const& mod : mods)
                        {
                            const char* opStr = mod.Op == AttributeModifier::Operation::Add ? "+" :
                                               mod.Op == AttributeModifier::Operation::Multiply ? "x" : "=";
                            ImGui::Text("%s %.1f [%s]", opStr, mod.Magnitude, mod.Source.GetTagString().c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }

                // Add attribute
                static char newAttrName[64] = "";
                static f32 newAttrValue = 0.0f;
                ImGui::Separator();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputText("##newAttrName", newAttrName, 64);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                ImGui::DragFloat("##newAttrValue", &newAttrValue, 0.1f);
                ImGui::SameLine();
                if (ImGui::SmallButton("Add Attribute") && newAttrName[0] != '\0')
                {
                    component.Attributes.DefineAttribute(newAttrName, newAttrValue);
                    newAttrName[0] = '\0';
                    newAttrValue = 0.0f;
                }
                ImGui::TreePop();
            }

            // Tags section
            if (ImGui::TreeNode("Owned Tags"))
            {
                for (auto const& tag : component.OwnedTags.GetTags())
                {
                    ImGui::Text("%s", tag.GetTagString().c_str());
                }
                static char newTag[128] = "";
                ImGui::InputText("##newTag", newTag, 128);
                ImGui::SameLine();
                if (ImGui::SmallButton("Add Tag") && newTag[0] != '\0')
                {
                    component.OwnedTags.AddTag(GameplayTag(std::string(newTag)));
                    newTag[0] = '\0';
                }
                ImGui::TreePop();
            }

            // Abilities section
            if (ImGui::TreeNode("Abilities"))
            {
                for (size_t i = 0; i < component.Abilities.size(); ++i)
                {
                    auto& ability = component.Abilities[i];
                    auto const& def = ability.Definition;
                    std::string label = def.Name.empty() ? ("Ability " + std::to_string(i)) : def.Name;
                    if (ability.IsActive)
                        label += " [ACTIVE]";

                    if (ImGui::TreeNode(("##ability" + std::to_string(i)).c_str(), "%s", label.c_str()))
                    {
                        ImGui::InputText("Name", &ability.Definition.Name);

                        static char tagBuf[128];
                        std::string tagStr = def.AbilityTag.GetTagString();
                        std::strncpy(tagBuf, tagStr.c_str(), sizeof(tagBuf) - 1);
                        tagBuf[sizeof(tagBuf) - 1] = '\0';
                        if (ImGui::InputText("Ability Tag", tagBuf, 128))
                        {
                            ability.Definition.AbilityTag = GameplayTag(std::string(tagBuf));
                        }

                        ImGui::DragFloat("Cooldown", &ability.Definition.CooldownDuration, 0.1f, 0.0f, 600.0f);
                        ImGui::DragFloat("Resource Cost", &ability.Definition.ResourceCost, 0.1f, 0.0f, 10000.0f);
                        ImGui::InputText("Cost Attribute", &ability.Definition.CostAttribute);
                        ImGui::Checkbox("Channeled", &ability.Definition.IsChanneled);
                        if (ability.Definition.IsChanneled)
                        {
                            ImGui::DragFloat("Channel Duration", &ability.Definition.ChannelDuration, 0.1f, 0.0f, 60.0f);
                        }
                        ImGui::Checkbox("Toggled", &ability.Definition.IsToggled);

                        // Tag arrays
                        auto drawTagList = [&i](const char* label, GameplayTagContainer& tagContainer, const char* id)
                        {
                            std::string treeId = std::string(label) + "##" + id + std::to_string(i);
                            if (ImGui::TreeNode(treeId.c_str(), "%s (%zu)", label, tagContainer.GetTags().size()))
                            {
                                auto const& tags = tagContainer.GetTags();
                                for (size_t t = 0; t < tags.size(); ++t)
                                {
                                    ImGui::Text("  %s", tags[t].GetTagString().c_str());
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton(("X##" + std::string(id) + std::to_string(i) + "_" + std::to_string(t)).c_str()))
                                    {
                                        tagContainer.RemoveTag(tags[t]);
                                        ImGui::TreePop();
                                        return;
                                    }
                                }
                                static char newTagBuf[128] = {};
                                ImGui::InputText(("##new" + std::string(id) + std::to_string(i)).c_str(), newTagBuf, 128);
                                ImGui::SameLine();
                                if (ImGui::SmallButton(("Add##" + std::string(id) + std::to_string(i)).c_str()) && newTagBuf[0] != '\0')
                                {
                                    tagContainer.AddTag(GameplayTag(std::string(newTagBuf)));
                                    newTagBuf[0] = '\0';
                                }
                                ImGui::TreePop();
                            }
                        };
                        drawTagList("Required Tags", ability.Definition.RequiredTags, "reqTag");
                        drawTagList("Blocked Tags", ability.Definition.BlockedTags, "blkTag");
                        drawTagList("Activation Granted Tags", ability.Definition.ActivationGrantedTags, "actTag");

                        // Activation Effects
                        if (std::string effectsTreeId = "Activation Effects##abilEffects" + std::to_string(i); ImGui::TreeNode(effectsTreeId.c_str(), "Activation Effects (%zu)", ability.Definition.ActivationEffects.size()))
                        {
                            for (size_t e = 0; e < ability.Definition.ActivationEffects.size(); ++e)
                            {
                                auto& effect = ability.Definition.ActivationEffects[e];
                                std::string effLabel = effect.Name.empty() ? ("Effect " + std::to_string(e)) : effect.Name;
                                std::string effId = "##abilEffect" + std::to_string(i) + "_" + std::to_string(e);
                                if (ImGui::TreeNode(effId.c_str(), "%s", effLabel.c_str()))
                                {
                                    ImGui::InputText("Name", &effect.Name);
                                    int durType = static_cast<int>(effect.Policy.DurationType);
                                    if (const char* durTypes[] = { "Instant", "HasDuration", "Infinite" }; ImGui::Combo("Duration Type", &durType, durTypes, 3))
                                    {
                                        effect.Policy.DurationType = static_cast<GameplayEffectPolicy::Duration>(durType);
                                    }
                                    if (effect.Policy.DurationType == GameplayEffectPolicy::Duration::HasDuration)
                                    {
                                        ImGui::DragFloat("Duration", &effect.Policy.DurationSeconds, 0.1f, 0.0f, 600.0f);
                                    }
                                    ImGui::Checkbox("Periodic", &effect.Policy.IsPeriodic);
                                    if (effect.Policy.IsPeriodic)
                                    {
                                        ImGui::DragFloat("Period", &effect.Policy.PeriodSeconds, 0.1f, 0.01f, 60.0f);
                                    }
                                    ImGui::DragInt("Max Stacks", &effect.MaxStacks, 1.0f, 1, 99);

                                    // Modifiers
                                    for (size_t m = 0; m < effect.Modifiers.size(); ++m)
                                    {
                                        auto& mod = effect.Modifiers[m];
                                        ImGui::PushID(static_cast<int>(m));
                                        ImGui::InputText("Attribute", &mod.AttributeName);
                                        int op = static_cast<int>(mod.Op);
                                        const char* ops[] = { "Add", "Multiply", "Override" };
                                        ImGui::Combo("Op", &op, ops, 3);
                                        mod.Op = static_cast<AttributeModifier::Operation>(op);
                                        ImGui::DragFloat("Magnitude", &mod.Magnitude, 0.1f);
                                        ImGui::SameLine();
                                        if (ImGui::SmallButton("X"))
                                        {
                                            effect.Modifiers.erase(effect.Modifiers.begin() + static_cast<ptrdiff_t>(m));
                                            ImGui::PopID();
                                            break;
                                        }
                                        ImGui::PopID();
                                    }
                                    if (ImGui::SmallButton("Add Modifier"))
                                    {
                                        effect.Modifiers.emplace_back();
                                    }

                                    ImGui::SameLine();
                                    if (ImGui::SmallButton(("Remove Effect##" + std::to_string(e)).c_str()))
                                    {
                                        ability.Definition.ActivationEffects.erase(ability.Definition.ActivationEffects.begin() + static_cast<ptrdiff_t>(e));
                                        ImGui::TreePop();
                                        ImGui::TreePop();
                                        ImGui::TreePop();
                                        ImGui::TreePop();
                                        return;
                                    }
                                    ImGui::TreePop();
                                }
                            }
                            if (ImGui::SmallButton("Add Effect"))
                            {
                                ability.Definition.ActivationEffects.emplace_back();
                            }
                            ImGui::TreePop();
                        }

                        // Target Activation Effects
                        if (std::string targetEffectsTreeId = "Target Effects##abilTargetEffects" + std::to_string(i); ImGui::TreeNode(targetEffectsTreeId.c_str(), "Target Effects (%zu)", ability.Definition.TargetActivationEffects.size()))
                        {
                            ImGui::TextDisabled("Applied to the target (via TryActivateAbilityOnTarget).");
                            ImGui::TextDisabled("If empty, ActivationEffects are used instead.");
                            for (size_t e = 0; e < ability.Definition.TargetActivationEffects.size(); ++e)
                            {
                                auto& effect = ability.Definition.TargetActivationEffects[e];
                                std::string effLabel = effect.Name.empty() ? ("Effect " + std::to_string(e)) : effect.Name;
                                std::string effId = "##abilTargetEffect" + std::to_string(i) + "_" + std::to_string(e);
                                if (ImGui::TreeNode(effId.c_str(), "%s", effLabel.c_str()))
                                {
                                    ImGui::InputText("Name", &effect.Name);
                                    int durType = static_cast<int>(effect.Policy.DurationType);
                                    if (const char* durTypes[] = { "Instant", "HasDuration", "Infinite" }; ImGui::Combo("Duration Type", &durType, durTypes, 3))
                                    {
                                        effect.Policy.DurationType = static_cast<GameplayEffectPolicy::Duration>(durType);
                                    }
                                    if (effect.Policy.DurationType == GameplayEffectPolicy::Duration::HasDuration)
                                    {
                                        ImGui::DragFloat("Duration", &effect.Policy.DurationSeconds, 0.1f, 0.0f, 600.0f);
                                    }
                                    ImGui::Checkbox("Periodic", &effect.Policy.IsPeriodic);
                                    if (effect.Policy.IsPeriodic)
                                    {
                                        ImGui::DragFloat("Period", &effect.Policy.PeriodSeconds, 0.1f, 0.01f, 60.0f);
                                    }
                                    ImGui::DragInt("Max Stacks", &effect.MaxStacks, 1.0f, 1, 99);

                                    for (size_t m = 0; m < effect.Modifiers.size(); ++m)
                                    {
                                        auto& mod = effect.Modifiers[m];
                                        ImGui::PushID(static_cast<int>(m));
                                        ImGui::InputText("Attribute", &mod.AttributeName);
                                        int op = static_cast<int>(mod.Op);
                                        const char* ops[] = { "Add", "Multiply", "Override" };
                                        ImGui::Combo("Op", &op, ops, 3);
                                        mod.Op = static_cast<AttributeModifier::Operation>(op);
                                        ImGui::DragFloat("Magnitude", &mod.Magnitude, 0.1f);
                                        ImGui::SameLine();
                                        if (ImGui::SmallButton("X"))
                                        {
                                            effect.Modifiers.erase(effect.Modifiers.begin() + static_cast<ptrdiff_t>(m));
                                            ImGui::PopID();
                                            break;
                                        }
                                        ImGui::PopID();
                                    }
                                    if (ImGui::SmallButton("Add Modifier"))
                                    {
                                        effect.Modifiers.emplace_back();
                                    }

                                    ImGui::SameLine();
                                    if (ImGui::SmallButton(("Remove Effect##t" + std::to_string(e)).c_str()))
                                    {
                                        ability.Definition.TargetActivationEffects.erase(ability.Definition.TargetActivationEffects.begin() + static_cast<ptrdiff_t>(e));
                                        ImGui::TreePop();
                                        ImGui::TreePop();
                                        ImGui::TreePop();
                                        ImGui::TreePop();
                                        return;
                                    }
                                    ImGui::TreePop();
                                }
                            }
                            if (ImGui::SmallButton("Add Target Effect"))
                            {
                                ability.Definition.TargetActivationEffects.emplace_back();
                            }
                            ImGui::TreePop();
                        }

                        // Show cooldown status
                        if (component.Cooldowns.IsOnCooldown(def.AbilityTag))
                        {
                            f32 remaining = component.Cooldowns.GetRemainingCooldown(def.AbilityTag);
                            f32 fraction = component.Cooldowns.GetCooldownFraction(def.AbilityTag);
                            ImGui::ProgressBar(fraction, ImVec2(-1, 0), ("CD: " + std::to_string(remaining) + "s").c_str());
                        }

                        ImGui::SameLine();
                        if (std::string removeLabel = "X##removeAbility" + std::to_string(i); ImGui::SmallButton(removeLabel.c_str()))
                        {
                            if (ability.IsActive)
                            {
                                GameplayAbilitySystem::CancelAbility(scene.get(), entity, ability.Definition.AbilityTag);
                            }
                            component.Cooldowns.ResetCooldown(ability.Definition.AbilityTag);
                            component.Abilities.erase(component.Abilities.begin() + static_cast<ptrdiff_t>(i));
                            ImGui::TreePop();
                            ImGui::TreePop();
                            return;
                        }

                        ImGui::TreePop();
                    }
                }
                if (ImGui::SmallButton("Add Ability"))
                {
                    component.Abilities.emplace_back();
                }
                ImGui::TreePop();
            }

            // Active Effects section
            if (ImGui::TreeNode("Active Effects"))
            {
                auto const& effects = component.ActiveEffects.GetActiveEffects();
                if (effects.empty())
                {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No active effects");
                }
                for (auto const& ae : effects)
                {
                    std::string durStr;
                    if (ae.Definition.Policy.DurationType == GameplayEffectPolicy::Duration::HasDuration)
                        durStr = std::to_string(ae.RemainingDuration) + "s remaining";
                    else if (ae.Definition.Policy.DurationType == GameplayEffectPolicy::Duration::Infinite)
                        durStr = "Infinite";

                    ImGui::Text("%s (x%d) %s", ae.Definition.Name.c_str(), ae.CurrentStacks, durStr.c_str());
                }
                ImGui::TreePop();
            }

            // Quick setup
            ImGui::Separator();
            if (ImGui::Button("Init Default RPG Attributes"))
            {
                component.InitializeDefaultRPGAttributes(100.0f, 50.0f, 10.0f, 5.0f);
            } });

        DrawComponent<IKTargetComponent>("IK Target", entity, [](auto& component)
                                         {
                ImGui::SeparatorText("Aim IK");
                ImGui::Checkbox("Aim Enabled", &component.AimIKEnabled);
                if (component.AimIKEnabled)
                {
                    if (auto aimBone = static_cast<int>(component.AimBoneIndex); ImGui::DragInt("Aim Bone Index", &aimBone, 1.0f, 0, 512))
                        component.AimBoneIndex = static_cast<u32>(aimBone);
                    ImGui::DragFloat3("Aim Target", glm::value_ptr(component.AimTarget), 0.1f);
                    ImGui::DragFloat3("Aim Axis", glm::value_ptr(component.AimAxis), 0.01f);
                    ImGui::DragFloat3("Aim Offset", glm::value_ptr(component.AimOffset), 0.01f);
                    ImGui::DragFloat3("Aim Pole Vector", glm::value_ptr(component.AimPoleVector), 0.01f);
                    if (auto aimLen = static_cast<int>(component.AimChainLength); ImGui::DragInt("Aim Chain Length", &aimLen, 1.0f, 1, 64))
                        component.AimChainLength = static_cast<u32>(aimLen);
                    ImGui::DragFloat("Aim Chain Factor", &component.AimChainFactor, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Aim Weight", &component.AimWeight, 0.01f, 0.0f, 1.0f);

                    auto aimTarget = static_cast<u64>(component.AimTargetEntity);
                    ImGui::Text("Aim Target Entity:");
                    ImGui::SameLine();
                    // Use a Button as a consistent drag-drop target
                    if (aimTarget != 0)
                    {
                        char label[64];
                        snprintf(label, sizeof(label), "%llu##AimTargetDrop", static_cast<unsigned long long>(aimTarget));
                        ImGui::Button(label);
                    }
                    else
                    {
                        ImGui::Button("(none — drag entity here)##AimTargetDrop");
                    }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (auto const* payload = ImGui::AcceptDragDropPayload("ENTITY_REPARENT"))
                        {
                            component.AimTargetEntity = *static_cast<const UUID*>(payload->Data);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (aimTarget != 0)
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear##AimTarget"))
                            component.AimTargetEntity = 0;
                    }
                }

                ImGui::SeparatorText("Limb IK");
                ImGui::Checkbox("Limb Enabled", &component.LimbIKEnabled);
                if (component.LimbIKEnabled)
                {
                    if (auto limbBone = static_cast<int>(component.LimbBoneIndex); ImGui::DragInt("Limb Bone Index", &limbBone, 1.0f, 0, 512))
                        component.LimbBoneIndex = static_cast<u32>(limbBone);
                    ImGui::DragFloat3("Limb Target", glm::value_ptr(component.LimbTarget), 0.1f);
                    if (auto limbLen = static_cast<int>(component.LimbChainLength); ImGui::DragInt("Limb Chain Length", &limbLen, 1.0f, 1, 64))
                        component.LimbChainLength = static_cast<u32>(limbLen);
                    ImGui::DragFloat("Limb Weight", &component.LimbWeight, 0.01f, 0.0f, 1.0f);

                    auto limbTarget = static_cast<u64>(component.LimbTargetEntity);
                    ImGui::Text("Limb Target Entity:");
                    ImGui::SameLine();
                    // Use a Button as a consistent drag-drop target
                    if (limbTarget != 0)
                    {
                        char label[64];
                        snprintf(label, sizeof(label), "%llu##LimbTargetDrop", static_cast<unsigned long long>(limbTarget));
                        ImGui::Button(label);
                    }
                    else
                    {
                        ImGui::Button("(none — drag entity here)##LimbTargetDrop");
                    }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (auto const* payload = ImGui::AcceptDragDropPayload("ENTITY_REPARENT"))
                        {
                            component.LimbTargetEntity = *static_cast<const UUID*>(payload->Data);
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (limbTarget != 0)
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear##LimbTarget"))
                            component.LimbTargetEntity = 0;
                    }
                } });
    }

    template<typename T>
    void SceneHierarchyPanel::DisplayAddComponentEntry(const std::string& entryName)
    {
        if ((!m_SelectionContext.HasComponent<T>()) && ImGui::MenuItem(entryName.c_str()))
        {
            if (m_CommandHistory)
            {
                m_CommandHistory->Execute(std::make_unique<AddComponentCommand<T>>(
                    m_Context, m_SelectionContext.GetUUID()));
            }
            else
            {
                m_SelectionContext.AddComponent<T>();
            }
            ImGui::CloseCurrentPopup();
        }
    }
} // namespace OloEngine
