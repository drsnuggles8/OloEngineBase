#include "OloEnginePCH.h"
#include "InstanceScatterBrushPanel.h"

#include "../UndoRedo/EditorCommand.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/InstancePlacementAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace OloEngine
{
    namespace
    {
        // Undo command for a single brush stroke. Captures the target
        // component's `Instances` vector before the stroke started; on Undo
        // the snapshot is swapped back. The post-stroke vector is captured
        // separately so Redo restores the painted state.
        class ScatterStrokeCommand : public EditorCommand
        {
          public:
            ScatterStrokeCommand(Ref<Scene> scene, UUID entityUUID,
                                 std::vector<InstanceData> preSnapshot,
                                 std::vector<InstanceData> postSnapshot,
                                 std::string description)
                : m_Scene(std::move(scene)),
                  m_EntityUUID(entityUUID),
                  m_PreSnapshot(std::move(preSnapshot)),
                  m_PostSnapshot(std::move(postSnapshot)),
                  m_Description(std::move(description))
            {
            }

            void Execute() override
            {
                ApplySnapshot(m_PostSnapshot);
            }
            void Undo() override
            {
                ApplySnapshot(m_PreSnapshot);
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return m_Description;
            }

          private:
            void ApplySnapshot(const std::vector<InstanceData>& snapshot)
            {
                if (!m_Scene)
                    return;
                // Resolve the entity by UUID at apply time — the editor may
                // have rebuilt the active scene between capture and undo
                // (e.g. play-mode entry/exit clones the entity registry).
                Entity entity = m_Scene->GetEntityByUUID(m_EntityUUID);
                if (!entity)
                    return;
                if (!entity.HasComponent<InstancedMeshComponent>())
                    return;
                auto& imc = entity.GetComponent<InstancedMeshComponent>();
                imc.Instances = snapshot;
                imc.InvalidateMergedCache();
            }

            Ref<Scene> m_Scene;
            UUID m_EntityUUID;
            std::vector<InstanceData> m_PreSnapshot;
            std::vector<InstanceData> m_PostSnapshot;
            std::string m_Description;
        };
    } // namespace

    void InstanceScatterBrushPanel::OnImGuiRender()
    {
        if (!Visible)
            return;

        ImGui::Begin("Instance Scatter Brush", &Visible);

        if (!m_Context)
        {
            ImGui::Text("No scene loaded.");
            ImGui::End();
            return;
        }

        // Mode tabs — mirrors TerrainEditorPanel for muscle memory.
        ImGui::Text("Mode:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Off", m_Mode == Mode::Off))
            m_Mode = Mode::Off;
        ImGui::SameLine();
        if (ImGui::RadioButton("Paint", m_Mode == Mode::Paint))
            m_Mode = Mode::Paint;

        ImGui::Separator();

        // Target entity — must carry an InstancedMeshComponent. Pulled from
        // the SceneHierarchyPanel selection by EditorLayer each frame; this
        // is a read-only label so the brush can paint without the user
        // needing to keep the inspector open.
        if (m_TargetEntity && m_TargetEntity.HasComponent<InstancedMeshComponent>())
        {
            static const std::string s_Unnamed("(unnamed)");
            const auto& tag = m_TargetEntity.HasComponent<TagComponent>()
                                  ? m_TargetEntity.GetComponent<TagComponent>().Tag
                                  : s_Unnamed; // never executed; HasComponent<Tag> always true on real entities
            ImGui::Text("Target: %s", tag.c_str());
            const auto& imc = m_TargetEntity.GetComponent<InstancedMeshComponent>();
            ImGui::Text("Inline placements: %zu", imc.Instances.size());
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                               "Select an entity with InstancedMeshComponent to paint.");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Brush");
        ImGui::DragFloat("Radius (m)", &m_BrushRadius, 0.1f, 0.1f, 200.0f, "%.2f");
        ImGui::SliderInt("Deposits / Tick", &m_DepositsPerTick, 1, 64);
        ImGui::DragFloat("Min Spacing (XZ)", &m_PoissonMinSpacing, 0.05f, 0.05f, 50.0f, "%.2f");
        ImGui::TextDisabled("Avoids stacking placements on existing ones in the brush footprint.");

        ImGui::Separator();
        ImGui::TextUnformatted("Placement");
        ImGui::DragFloatRange2("Scale", &m_ScaleMin, &m_ScaleMax, 0.01f, 0.01f, 10.0f);
        ImGui::Checkbox("Random Y Rotation", &m_RandomYRot);
        ImGui::Checkbox("Align Y to Surface Normal", &m_AlignToNormal);
        ImGui::TextDisabled("Off = gravity-aligned (foliage default).");

        ImGui::Separator();
        ImGui::TextUnformatted("Filters");
        ImGui::SliderFloat("Min Up-Dot (slope)", &m_SlopeMinDot, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("0.71 ≈ reject slopes steeper than 45°. 0 disables.");

        ImGui::Separator();
        ImGui::TextUnformatted("Variants");
        ImGui::SliderInt("Variant Count", &m_VariantCount, 1, 16);
        ImGui::TextDisabled("Variant index stored in InstanceData.Custom for shader-side branching.");

        ImGui::Separator();
        ImGui::TextUnformatted("Bake to Asset");
        // Stable default path on first paint.
        if (!m_BakePathInitialised)
        {
            const char* def = "scatter/scatter_baked.oloinstances";
            std::strncpy(m_BakePathBuf.data(), def, m_BakePathBuf.size() - 1);
            m_BakePathBuf.back() = '\0';
            m_BakePathInitialised = true;
        }
        ImGui::InputText("##BakePath", m_BakePathBuf.data(), m_BakePathBuf.size());
        ImGui::SameLine();
        const bool canBake = m_TargetEntity && m_TargetEntity.HasComponent<InstancedMeshComponent>() &&
                             !m_TargetEntity.GetComponent<InstancedMeshComponent>().Instances.empty();
        ImGui::BeginDisabled(!canBake);
        if (ImGui::Button("Bake"))
        {
            // Build the asset from the inline list, write it to disk via
            // EditorAssetManager, hook up `PlacementAssetHandle`, and clear
            // the inline list — matching the §1.7 workflow spec.
            auto& imc = m_TargetEntity.GetComponent<InstancedMeshComponent>();
            const std::filesystem::path relativePath{ m_BakePathBuf.data() };
            const std::filesystem::path fullPath = Project::GetAssetDirectory() / relativePath;

            // Snapshot for undo: before-state covers both the inline list
            // and the placement-handle slot (both change after the bake).
            std::vector<InstanceData> preSnapshot = imc.Instances;
            AssetHandle prePlacementHandle = imc.PlacementAssetHandle;

            auto editorMgr = Project::GetAssetManager().As<EditorAssetManager>();
            if (!editorMgr)
            {
                OLO_CORE_ERROR("InstanceScatterBrush: No editor asset manager — bake only works in the editor.");
            }
            else
            {
                // CreateOrReplaceAsset forwards ctor args; the new
                // InstancePlacementAsset(vector) ctor lets it serialise the
                // painted placements to disk in one shot.
                auto asset = editorMgr->CreateOrReplaceAsset<InstancePlacementAsset>(
                    fullPath, imc.Instances);
                if (asset)
                {
                    imc.PlacementAssetHandle = asset->GetHandle();
                    imc.Instances.clear();
                    imc.InvalidateMergedCache();

                    if (m_CommandHistory)
                    {
                        // Bake is its own undoable step — Ctrl+Z restores
                        // the inline list and clears the placement handle.
                        // We piggy-back on ScatterStrokeCommand by encoding
                        // the post-bake state (empty inline) into the
                        // post-snapshot; placement handle is captured via a
                        // sibling lambda command pushed as compound.
                        class BakeCommand : public EditorCommand
                        {
                          public:
                            BakeCommand(Ref<Scene> scene, UUID entityUUID,
                                        std::vector<InstanceData> preInstances,
                                        AssetHandle prePlacementHandle,
                                        AssetHandle postPlacementHandle)
                                : m_Scene(std::move(scene)), m_EntityUUID(entityUUID),
                                  m_PreInstances(std::move(preInstances)),
                                  m_PrePlacementHandle(prePlacementHandle),
                                  m_PostPlacementHandle(postPlacementHandle)
                            {
                            }
                            void Execute() override
                            {
                                Apply(m_PostPlacementHandle, true);
                            }
                            void Undo() override
                            {
                                Apply(m_PrePlacementHandle, false);
                            }
                            [[nodiscard]] std::string GetDescription() const override
                            {
                                return "Bake Scatter to .oloinstances";
                            }

                          private:
                            void Apply(AssetHandle handle, bool clearInline)
                            {
                                if (!m_Scene)
                                    return;
                                Entity e = m_Scene->GetEntityByUUID(m_EntityUUID);
                                if (!e || !e.HasComponent<InstancedMeshComponent>())
                                    return;
                                auto& imc = e.GetComponent<InstancedMeshComponent>();
                                imc.PlacementAssetHandle = handle;
                                if (clearInline)
                                    imc.Instances.clear();
                                else
                                    imc.Instances = m_PreInstances;
                                imc.InvalidateMergedCache();
                            }
                            Ref<Scene> m_Scene;
                            UUID m_EntityUUID;
                            std::vector<InstanceData> m_PreInstances;
                            AssetHandle m_PrePlacementHandle, m_PostPlacementHandle;
                        };
                        m_CommandHistory->PushAlreadyExecuted(
                            std::make_unique<BakeCommand>(m_Context, m_TargetEntity.GetUUID(),
                                                          std::move(preSnapshot), prePlacementHandle, asset->GetHandle()));
                    }

                    OLO_CORE_INFO("InstanceScatterBrush: Baked {} placements to {}",
                                  asset->GetInstances().size(), fullPath.string());
                }
                else
                {
                    OLO_CORE_ERROR("InstanceScatterBrush: CreateOrReplaceAsset failed for path: {}", fullPath.string());
                }
            }
        }
        ImGui::EndDisabled();
        if (!canBake)
            ImGui::TextDisabled("(target must have non-empty inline placements)");

        ImGui::End();
    }

    void InstanceScatterBrushPanel::OnUpdate(f32 deltaTime, const glm::vec3& hitPos,
                                             const glm::vec3& surfaceNormal,
                                             bool hasHit, bool mouseDown)
    {
        (void)deltaTime;
        m_BrushWorldPos = hitPos;
        m_BrushSurfaceNormal = (glm::length(surfaceNormal) > 0.0001f) ? glm::normalize(surfaceNormal)
                                                                      : glm::vec3(0.0f, 1.0f, 0.0f);
        m_HasBrushHit = hasHit;

        if (!IsActive())
        {
            // Mouse-down transition while inactive doesn't start a stroke;
            // also ensures a half-finished stroke is closed if the user
            // toggles Off mid-drag.
            if (m_StrokeActive)
                EndStroke();
            m_PrevMouseDown = false;
            return;
        }

        const bool mouseDownThisFrame = mouseDown && hasHit;

        // Mouse-down edge → begin stroke + deposit immediately so a single
        // click without a drag still produces placements.
        if (mouseDownThisFrame && !m_PrevMouseDown)
            BeginStroke();

        if (mouseDownThisFrame && m_StrokeActive)
            DepositStrokeTick(hitPos, m_BrushSurfaceNormal);

        // Mouse-up edge → close the stroke and push undo command.
        if (!mouseDown && m_PrevMouseDown && m_StrokeActive)
            EndStroke();

        m_PrevMouseDown = mouseDown;
    }

    void InstanceScatterBrushPanel::BeginStroke()
    {
        if (!m_TargetEntity || !m_TargetEntity.HasComponent<InstancedMeshComponent>())
            return;
        const auto& imc = m_TargetEntity.GetComponent<InstancedMeshComponent>();
        m_StrokePreSnapshot = imc.Instances;
        m_StrokeActive = true;
    }

    void InstanceScatterBrushPanel::EndStroke()
    {
        if (!m_StrokeActive)
            return;
        m_StrokeActive = false;
        if (!m_CommandHistory || !m_TargetEntity || !m_TargetEntity.HasComponent<InstancedMeshComponent>())
            return;
        const auto& imc = m_TargetEntity.GetComponent<InstancedMeshComponent>();
        if (m_StrokePreSnapshot.size() == imc.Instances.size())
            return; // nothing deposited (e.g. slope filter rejected everything) — skip the undo entry

        std::vector<InstanceData> postSnapshot = imc.Instances;
        m_CommandHistory->PushAlreadyExecuted(std::make_unique<ScatterStrokeCommand>(
            m_Context, m_TargetEntity.GetUUID(),
            std::move(m_StrokePreSnapshot), std::move(postSnapshot),
            "Scatter Brush Stroke"));
        m_StrokePreSnapshot.clear();
    }

    void InstanceScatterBrushPanel::DepositStrokeTick(const glm::vec3& centre, const glm::vec3& surfaceNormal)
    {
        if (!m_TargetEntity || !m_TargetEntity.HasComponent<InstancedMeshComponent>())
            return;
        auto& imc = m_TargetEntity.GetComponent<InstancedMeshComponent>();

        // Slope filter at the brush centre — when the surface at the cursor
        // is too steep, the entire tick is skipped rather than rejecting
        // individual candidates. Cheaper, and matches the user's mental
        // model ("brush doesn't paint on cliffs").
        if (m_SlopeMinDot > 0.0f && glm::dot(surfaceNormal, glm::vec3(0.0f, 1.0f, 0.0f)) < m_SlopeMinDot)
            return;

        thread_local std::mt19937 rng{ std::random_device{}() };
        std::uniform_real_distribution<f32> distAngle(0.0f, 6.2831853f);
        std::uniform_real_distribution<f32> distR2(0.0f, m_BrushRadius * m_BrushRadius);
        std::uniform_real_distribution<f32> distScale(m_ScaleMin, m_ScaleMax);
        std::uniform_real_distribution<f32> distVariant(0.0f, static_cast<f32>(std::max(1, m_VariantCount)));

        const f32 minSpacingSq = m_PoissonMinSpacing * m_PoissonMinSpacing;
        for (i32 i = 0; i < m_DepositsPerTick; ++i)
        {
            // Uniform disc sampling: angle uniform, radius = sqrt(uniform(0, r²)).
            // Naive uniform(0, r) clusters at the centre.
            const f32 angle = distAngle(rng);
            const f32 radius = std::sqrt(distR2(rng));
            const glm::vec3 pos = centre + glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);

            // Rejection: too close to any existing placement on the XZ
            // plane. O(N) per candidate — acceptable for brush tick sizes
            // (~8-30 per frame). A grid accelerator would be necessary if
            // we cross ~10k inline placements with a small min-spacing.
            bool tooClose = false;
            for (const auto& existing : imc.Instances)
            {
                const glm::vec3 ep = glm::vec3(existing.Transform[3]);
                const f32 dx = ep.x - pos.x;
                const f32 dz = ep.z - pos.z;
                if (dx * dx + dz * dz < minSpacingSq)
                {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose)
                continue;

            const f32 scale = distScale(rng);
            const f32 yaw = m_RandomYRot ? distAngle(rng) : 0.0f;

            // Build the placement transform. Optionally align local Y to
            // the surface normal — useful for debris on slopes; off by
            // default since foliage typically stays gravity-aligned.
            glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
            glm::mat4 r = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0));
            glm::mat4 s = glm::scale(glm::mat4(1.0f), glm::vec3(scale));
            glm::mat4 normalAlign(1.0f);
            if (m_AlignToNormal)
            {
                const glm::vec3 up = glm::vec3(0, 1, 0);
                const glm::vec3 axis = glm::cross(up, surfaceNormal);
                const f32 axisLen = glm::length(axis);
                if (axisLen > 0.0001f)
                {
                    const f32 cosA = glm::clamp(glm::dot(up, surfaceNormal), -1.0f, 1.0f);
                    normalAlign = glm::rotate(glm::mat4(1.0f), std::acos(cosA), axis / axisLen);
                }
            }

            InstanceData inst;
            inst.Transform = t * normalAlign * r * s;
            inst.Normal = glm::transpose(glm::inverse(inst.Transform));
            inst.PrevTransform = inst.Transform;
            // Variant index as a normalised float so the existing
            // InstanceData.Custom slot suffices — shader can decode via
            // `i32(Custom * variantCount)` and select a sub-mesh / submat.
            const i32 variantIdx = static_cast<i32>(distVariant(rng));
            const i32 variantClamped = std::clamp(variantIdx, 0, std::max(0, m_VariantCount - 1));
            inst.Custom = (m_VariantCount > 1) ? static_cast<f32>(variantClamped) / static_cast<f32>(m_VariantCount - 1)
                                               : 0.0f;
            imc.Instances.push_back(inst);
        }
        imc.InvalidateMergedCache();
    }
} // namespace OloEngine
