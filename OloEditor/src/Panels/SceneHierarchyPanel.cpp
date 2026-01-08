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

            // Animation Components
            DisplayAddComponentEntry<AnimationStateComponent>("Animation State");
            DisplayAddComponentEntry<SkeletonComponent>("Skeleton");
            DisplayAddComponentEntry<SubmeshComponent>("Submesh");

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
