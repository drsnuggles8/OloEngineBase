#include "OloEnginePCH.h"
#include "TerrainEditorPanel.h"
#include "OloEngine/Core/FastRandom.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/Editor/TerrainErosion.h"
#include "OloEngine/Terrain/Editor/TerrainGPUBrush.h"
#include "OloEngine/Terrain/Editor/TerrainTextureUndoStack.h"
#include "../UndoRedo/SpecializedCommands.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace OloEngine
{
    static const char* s_SculptToolNames[] = { "Raise", "Lower", "Smooth", "Flatten", "Level" };

    void TerrainEditorPanel::OnImGuiRender()
    {
        if (!Visible)
            return;

        ImGui::Begin("Terrain Editor", &Visible);

        if (!m_Context)
        {
            ImGui::Text("No scene loaded.");
            ImGui::End();
            return;
        }

        // Check if any terrain exists in scene
        auto terrainView = m_Context->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
        if (const bool hasTerrain = terrainView.begin() != terrainView.end(); !hasTerrain)
        {
            ImGui::Text("No terrain in scene.");
            ImGui::End();
            return;
        }

        // Mode tabs
        ImGui::Text("Edit Mode:");
        ImGui::SameLine();
        if (ImGui::RadioButton("None", m_EditMode == TerrainEditMode::None))
            m_EditMode = TerrainEditMode::None;
        ImGui::SameLine();
        if (ImGui::RadioButton("Generate", m_EditMode == TerrainEditMode::Generate))
            m_EditMode = TerrainEditMode::Generate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Sculpt", m_EditMode == TerrainEditMode::Sculpt))
            m_EditMode = TerrainEditMode::Sculpt;
        ImGui::SameLine();
        if (ImGui::RadioButton("Paint", m_EditMode == TerrainEditMode::Paint))
            m_EditMode = TerrainEditMode::Paint;
        ImGui::SameLine();
        if (ImGui::RadioButton("Voxel", m_EditMode == TerrainEditMode::Voxel))
            m_EditMode = TerrainEditMode::Voxel;
        ImGui::SameLine();
        if (ImGui::RadioButton("Erosion", m_EditMode == TerrainEditMode::Erosion))
            m_EditMode = TerrainEditMode::Erosion;

        ImGui::Separator();

        switch (m_EditMode)
        {
            case TerrainEditMode::Generate:
                DrawGenerateUI();
                break;
            case TerrainEditMode::Sculpt:
                DrawSculptUI();
                break;
            case TerrainEditMode::Paint:
                DrawPaintUI();
                break;
            case TerrainEditMode::Voxel:
                DrawVoxelUI();
                break;
            case TerrainEditMode::Erosion:
                DrawErosionUI();
                break;
            default:
                ImGui::Text("Select a mode to begin editing.");
                break;
        }

        // Import / Export section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Import / Export"))
        {
            for (auto entity : terrainView)
            {
                auto& tc = terrainView.get<TerrainComponent>(entity);
                if (!tc.m_TerrainData)
                    continue;

                if (ImGui::Button("Export R32F (.raw)"))
                {
                    tc.m_TerrainData->ExportRawR32F("assets/terrain/heightmap_export.r32f");
                }
                ImGui::SameLine();
                if (ImGui::Button("Export R16 (.raw)"))
                {
                    tc.m_TerrainData->ExportRawR16("assets/terrain/heightmap_export.r16");
                }
                break;
            }
        }

        ImGui::End();
    }

    void TerrainEditorPanel::DrawGenerateUI()
    {
        // Operate on the first terrain in the scene.
        auto terrainView = m_Context->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
        TerrainComponent* terrainPtr = nullptr;
        for (auto entity : terrainView)
        {
            if (terrainPtr == nullptr)
                terrainPtr = &terrainView.get<TerrainComponent>(entity);
        }
        if (!terrainPtr)
        {
            ImGui::TextDisabled("No terrain in scene.");
            return;
        }
        TerrainComponent& tc = *terrainPtr;

        // Re-noise the height field (and re-derive the auto-material) on the next
        // tick. Dropping m_TerrainData forces Scene to regenerate from scratch —
        // editing the procedural params alone only rebuilds chunks from the cached
        // heightmap, so the shape would not change without this.
        const auto regenerate = [&tc]()
        {
            tc.m_TerrainData = nullptr;
            tc.m_NeedsRebuild = true;
            tc.m_AutoSplatNeedsRebuild = true;
        };

        if (!tc.m_ProceduralEnabled)
        {
            ImGui::TextWrapped("This terrain is not procedural. Enable it to generate a height field from noise.");
            if (ImGui::Button("Enable Procedural Generation"))
            {
                tc.m_ProceduralEnabled = true;
                regenerate();
            }
            return;
        }

        // ── Shape presets ────────────────────────────────────────────────────
        // One-click terrain archetypes — each sets the shaping knobs (and base
        // frequency) then regenerates. The starting point for "playing around".
        struct ShapePreset
        {
            const char* Name;
            f32 Ridge;
            f32 Warp;
            f32 WarpFreq;
            u32 Terrace;
            f32 TerraceSharp;
            f32 Exponent;
            f32 Frequency;
            f32 Falloff;
            f32 FalloffRadius;
        };
        // "Islands" is the only preset that turns the radial mask on: without it
        // the tile border keeps whatever height the noise left there, which is a
        // cliff wall rather than a shoreline (issue #880).
        static const ShapePreset kPresets[] = {
            { "Rolling Hills", 0.0f, 0.06f, 2.0f, 0, 0.6f, 1.0f, 2.5f, 0.0f, 0.3f },
            { "Mountains", 0.6f, 0.15f, 2.0f, 0, 0.6f, 1.2f, 3.0f, 0.0f, 0.3f },
            { "Mesas", 0.2f, 0.10f, 2.0f, 6, 0.85f, 1.0f, 2.5f, 0.0f, 0.3f },
            { "Islands", 0.3f, 0.10f, 2.0f, 0, 0.6f, 1.3f, 2.4f, 1.0f, 0.30f },
            { "Canyons", 0.7f, 0.25f, 3.0f, 0, 0.6f, 1.5f, 3.5f, 0.0f, 0.3f },
        };
        ImGui::Text("Shape Presets");
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i)
        {
            const ShapePreset& p = kPresets[i];
            if (i > 0)
                ImGui::SameLine();
            if (ImGui::Button(p.Name))
            {
                tc.m_HeightShaping.RidgeBlend = p.Ridge;
                tc.m_HeightShaping.WarpStrength = p.Warp;
                tc.m_HeightShaping.WarpFrequency = p.WarpFreq;
                tc.m_HeightShaping.TerraceSteps = p.Terrace;
                tc.m_HeightShaping.TerraceSharpness = p.TerraceSharp;
                tc.m_HeightShaping.HeightExponent = p.Exponent;
                tc.m_HeightShaping.IslandFalloff = p.Falloff;
                tc.m_HeightShaping.IslandFalloffRadius = p.FalloffRadius;
                tc.m_ProceduralFrequency = p.Frequency;
                regenerate();
            }
        }

        ImGui::Separator();

        // ── Base noise ───────────────────────────────────────────────────────
        ImGui::Text("Noise");
        ImGui::DragInt("Seed", &tc.m_ProceduralSeed, 1);
        ImGui::SameLine();
        if (ImGui::Button("Randomize"))
        {
            tc.m_ProceduralSeed = RandomUtils::Int32(0, std::numeric_limits<i32>::max());
            regenerate();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Picks a new seed and regenerates immediately.\nEditing the Seed value above instead requires the Generate / Regenerate button.");
        if (int res = static_cast<int>(tc.m_ProceduralResolution); ImGui::DragInt("Resolution", &res, 1, 64, 2048))
            tc.m_ProceduralResolution = static_cast<u32>(std::clamp(res, 64, 2048));
        if (int oct = static_cast<int>(tc.m_ProceduralOctaves); ImGui::DragInt("Octaves", &oct, 1, 1, 12))
            tc.m_ProceduralOctaves = static_cast<u32>(std::clamp(oct, 1, 12));
        ImGui::DragFloat("Frequency", &tc.m_ProceduralFrequency, 0.1f, 0.1f, 20.0f, "%.2f");
        ImGui::DragFloat("Lacunarity", &tc.m_ProceduralLacunarity, 0.05f, 1.0f, 4.0f, "%.2f");
        ImGui::DragFloat("Persistence", &tc.m_ProceduralPersistence, 0.01f, 0.1f, 0.9f, "%.2f");

        // ── Shaping ──────────────────────────────────────────────────────────
        ImGui::Separator();
        ImGui::Text("Shaping");
        ImGui::DragFloat("Ridge Blend", &tc.m_HeightShaping.RidgeBlend, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("0 = rolling fBm hills, 1 = sharp ridged mountains");
        ImGui::DragFloat("Warp Strength", &tc.m_HeightShaping.WarpStrength, 0.005f, 0.0f, 1.0f, "%.3f");
        ImGui::SetItemTooltip("Domain warp — meandering ridges instead of a grid-aligned look");
        ImGui::DragFloat("Warp Frequency", &tc.m_HeightShaping.WarpFrequency, 0.05f, 0.1f, 16.0f, "%.2f");
        if (int steps = static_cast<int>(tc.m_HeightShaping.TerraceSteps); ImGui::DragInt("Terrace Steps", &steps, 0.2f, 0, 32))
            tc.m_HeightShaping.TerraceSteps = static_cast<u32>(std::max(0, steps));
        ImGui::SetItemTooltip("0 = off; flat plateaus (mesa look)");
        ImGui::DragFloat("Terrace Sharpness", &tc.m_HeightShaping.TerraceSharpness, 0.01f, 0.0f, 0.99f, "%.2f");
        ImGui::DragFloat("Height Exponent", &tc.m_HeightShaping.HeightExponent, 0.02f, 0.1f, 6.0f, "%.2f");
        ImGui::SetItemTooltip(">1 flattens lowlands and sharpens peaks (islands / deep valleys)");
        ImGui::DragFloat("Island Falloff", &tc.m_HeightShaping.IslandFalloff, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("0 = off. Radial mask driving the tile border down to the base height, so the tile edge is a shoreline instead of a cliff.");
        ImGui::DragFloat("Island Falloff Radius", &tc.m_HeightShaping.IslandFalloffRadius, 0.005f, 0.0f, 0.5f, "%.3f");
        ImGui::SetItemTooltip("Normalized radius the mask starts falling at. Smaller = a smaller island in the same tile.");

        // ── World ────────────────────────────────────────────────────────────
        ImGui::Separator();
        ImGui::Text("World");
        ImGui::DragFloat("World Size X", &tc.m_WorldSizeX, 1.0f, 1.0f, 16384.0f, "%.0f");
        ImGui::DragFloat("World Size Z", &tc.m_WorldSizeZ, 1.0f, 1.0f, 16384.0f, "%.0f");
        ImGui::DragFloat("Height Scale", &tc.m_HeightScale, 0.5f, 0.0f, 1024.0f, "%.1f");

        // ── Auto-material ────────────────────────────────────────────────────
        ImGui::Separator();
        ImGui::Text("Auto-Material");
        if (ImGui::Checkbox("Auto Material from Rules", &tc.m_AutoMaterial))
            tc.m_AutoSplatNeedsRebuild = true;
        if (tc.m_AutoMaterial)
        {
            if (ImGui::Button("Apply Default Biome Preset"))
            {
                if (!tc.m_Material)
                    tc.m_Material = Ref<TerrainMaterial>::Create();
                while (tc.m_Material->GetLayerCount() > 0)
                    tc.m_Material->RemoveLayer(0);
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    tc.m_Material->AddLayer(layer);
                tc.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                tc.m_MaterialNeedsRebuild = true;
                tc.m_AutoSplatNeedsRebuild = true;
            }
            if (int splatRes = static_cast<int>(tc.m_SplatmapGenResolution); ImGui::DragInt("Splatmap Resolution", &splatRes, 1.0f, 64, 2048))
            {
                tc.m_SplatmapGenResolution = static_cast<u32>(std::clamp(splatRes, 64, 2048));
                tc.m_AutoSplatNeedsRebuild = true;
            }

            // Compact per-rule band editor (LayerIndex + height/slope ranges).
            for (sizet i = 0; i < tc.m_LayerRules.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                TerrainLayerRule& rule = tc.m_LayerRules[i];
                const char* layerName = (tc.m_Material && rule.LayerIndex < tc.m_Material->GetLayerCount())
                                            ? tc.m_Material->GetLayer(rule.LayerIndex).Name.c_str()
                                            : "Layer";
                if (ImGui::TreeNodeEx(layerName, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (int li = static_cast<int>(rule.LayerIndex); ImGui::DragInt("Layer", &li, 0.1f, 0, static_cast<int>(MAX_TERRAIN_LAYERS) - 1))
                    {
                        rule.LayerIndex = static_cast<u32>(std::clamp(li, 0, static_cast<int>(MAX_TERRAIN_LAYERS) - 1));
                        tc.m_AutoSplatNeedsRebuild = true;
                    }
                    if (ImGui::DragFloatRange2("Height", &rule.MinHeight, &rule.MaxHeight, 0.005f, 0.0f, 1.0f, "%.2f"))
                        tc.m_AutoSplatNeedsRebuild = true;
                    if (ImGui::DragFloatRange2("Slope", &rule.MinSlopeDeg, &rule.MaxSlopeDeg, 0.5f, 0.0f, 90.0f, "%.0f"))
                        tc.m_AutoSplatNeedsRebuild = true;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Generate Splatmap Now"))
                tc.m_AutoSplatNeedsRebuild = true;
        }

        // ── Generate ─────────────────────────────────────────────────────────
        ImGui::Separator();
        if (ImGui::Button("Generate / Regenerate", ImVec2(-1.0f, 0.0f)))
            regenerate();
        if (tc.m_TerrainData)
            ImGui::TextDisabled("Resolution %u x %u", tc.m_TerrainData->GetResolution(), tc.m_TerrainData->GetResolution());
    }

    void TerrainEditorPanel::DrawSculptUI()
    {
        ImGui::Text("Sculpt Tool");

        if (int currentTool = static_cast<int>(std::to_underlying(m_SculptSettings.Tool)); ImGui::Combo("Tool", &currentTool, s_SculptToolNames, IM_ARRAYSIZE(s_SculptToolNames)))
            m_SculptSettings.Tool = static_cast<TerrainBrushTool>(currentTool);

        ImGui::DragFloat("Radius", &m_SculptSettings.Radius, 0.5f, 0.5f, 200.0f, "%.1f");
        ImGui::DragFloat("Strength", &m_SculptSettings.Strength, 0.01f, 0.01f, 5.0f, "%.2f");
        ImGui::DragFloat("Falloff", &m_SculptSettings.Falloff, 0.01f, 0.0f, 1.0f, "%.2f");

        ImGui::Separator();
        if (m_HasBrushHit)
        {
            ImGui::Text("Hit: (%.1f, %.1f, %.1f)", m_BrushWorldPos.x, m_BrushWorldPos.y, m_BrushWorldPos.z);
        }
        else
        {
            ImGui::TextDisabled("Hover terrain to begin sculpting");
        }
    }

    void TerrainEditorPanel::DrawPaintUI()
    {
        ImGui::Text("Paint Tool");

        // Find first terrain with material to get layer names
        auto terrainView = m_Context->GetAllEntitiesWith<TerrainComponent>();
        u32 maxLayers = 0;
        for (auto entity : terrainView)
        {
            auto& tc = terrainView.get<TerrainComponent>(entity);
            if (tc.m_Material)
            {
                maxLayers = tc.m_Material->GetLayerCount();
                break;
            }
        }

        if (maxLayers == 0)
        {
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "No material layers. Add layers in the component panel first.");
            return;
        }

        if (int targetLayer = static_cast<int>(m_PaintSettings.TargetLayer); ImGui::SliderInt("Target Layer", &targetLayer, 0, static_cast<int>(maxLayers - 1)))
            m_PaintSettings.TargetLayer = static_cast<u32>(targetLayer);

        // Show layer name
        for (auto entity : terrainView)
        {
            auto& tc = terrainView.get<TerrainComponent>(entity);
            if (tc.m_Material && m_PaintSettings.TargetLayer < tc.m_Material->GetLayerCount())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", tc.m_Material->GetLayer(m_PaintSettings.TargetLayer).Name.c_str());
                break;
            }
        }

        ImGui::DragFloat("Radius", &m_PaintSettings.Radius, 0.5f, 0.5f, 200.0f, "%.1f");
        ImGui::DragFloat("Strength", &m_PaintSettings.Strength, 0.01f, 0.01f, 5.0f, "%.2f");
        ImGui::DragFloat("Falloff", &m_PaintSettings.Falloff, 0.01f, 0.0f, 1.0f, "%.2f");

        ImGui::Separator();
        if (m_HasBrushHit)
        {
            ImGui::Text("Hit: (%.1f, %.1f, %.1f)", m_BrushWorldPos.x, m_BrushWorldPos.y, m_BrushWorldPos.z);
        }
        else
        {
            ImGui::TextDisabled("Hover terrain to begin painting");
        }
    }

    void TerrainEditorPanel::OnFrameTick(bool editingAllowed)
    {
        if (!editingAllowed)
        {
            // Not a reason to return early: an in-flight session must still settle,
            // or its undo entry is never pushed and it keeps the terrain alive.
            if (m_ErosionSessionActive)
            {
                EndContinuousErosionSession();
            }
            return;
        }

        UpdateContinuousErosion();
    }

    TerrainTextureUndoStack& TerrainEditorPanel::EnsureUndoStack()
    {
        if (!m_UndoStack)
        {
            m_UndoStack = Ref<TerrainTextureUndoStack>::Create();
        }
        return *m_UndoStack;
    }

    void TerrainEditorPanel::CommitGPUSculptStroke()
    {
        if (!m_CommandHistory || !m_StrokeTerrainData || !m_StrokePreHeight)
        {
            return;
        }

        Ref<Texture2D> heightmap = m_StrokeTerrainData->GetGPUHeightmap();
        if (!heightmap)
        {
            return;
        }

        TerrainTextureUndoStack& stack = EnsureUndoStack();
        const auto before = stack.Capture(m_StrokePreHeight, m_StrokeDirtyX, m_StrokeDirtyY,
                                          m_StrokeDirtyW, m_StrokeDirtyH);
        const auto after = stack.Capture(heightmap, m_StrokeDirtyX, m_StrokeDirtyY,
                                         m_StrokeDirtyW, m_StrokeDirtyH);

        // Both halves or neither: a command holding only one of the pair would undo
        // to nothing or redo to nothing, which is worse than having no undo entry.
        if (before == TerrainTextureUndoStack::kInvalidSnapshot ||
            after == TerrainTextureUndoStack::kInvalidSnapshot)
        {
            stack.Release(before);
            stack.Release(after);
            OLO_CORE_WARN("TerrainEditorPanel: could not snapshot the sculpt stroke — no undo entry recorded");
            return;
        }

        Entity strokeEnt = (m_Context && m_StrokeEntity != entt::null)
                               ? Entity{ m_StrokeEntity, m_Context.get() }
                               : Entity{};
        const UUID strokeUUID = strokeEnt ? strokeEnt.GetUUID() : UUID(0);

        m_CommandHistory->PushAlreadyExecuted(
            std::make_unique<TerrainGPUSculptCommand>(
                m_StrokeTerrainData, m_StrokeChunkManager, m_UndoStack,
                m_StrokeWorldSizeX, m_StrokeWorldSizeZ, m_StrokeHeightScale,
                m_StrokeDirtyX, m_StrokeDirtyY, m_StrokeDirtyW, m_StrokeDirtyH,
                before, after, WeakRef<Scene>(m_Context), strokeUUID));
    }

    void TerrainEditorPanel::CommitGPUPaintStroke()
    {
        if (!m_CommandHistory || !m_StrokeMaterial || !m_StrokePreSplat0)
        {
            return;
        }

        Ref<Texture2D> splat0 = m_StrokeMaterial->GetSplatmap(0);
        if (!splat0)
        {
            return;
        }

        TerrainTextureUndoStack& stack = EnsureUndoStack();
        const auto before0 = stack.Capture(m_StrokePreSplat0, m_StrokeDirtyX, m_StrokeDirtyY,
                                           m_StrokeDirtyW, m_StrokeDirtyH);
        const auto after0 = stack.Capture(splat0, m_StrokeDirtyX, m_StrokeDirtyY,
                                          m_StrokeDirtyW, m_StrokeDirtyH);

        auto before1 = TerrainTextureUndoStack::kInvalidSnapshot;
        auto after1 = TerrainTextureUndoStack::kInvalidSnapshot;
        // The second splatmap only participates above four layers — that is exactly
        // when the paint kernel re-normalises across both, so it is exactly when it
        // needs undoing too.
        const bool needsSplat1 = m_StrokeMaterial->GetLayerCount() > 4 && m_StrokePreSplat1 &&
                                 m_StrokeMaterial->GetSplatmap(1) != nullptr;
        if (needsSplat1)
        {
            before1 = stack.Capture(m_StrokePreSplat1, m_StrokeDirtyX, m_StrokeDirtyY,
                                    m_StrokeDirtyW, m_StrokeDirtyH);
            after1 = stack.Capture(m_StrokeMaterial->GetSplatmap(1), m_StrokeDirtyX, m_StrokeDirtyY,
                                   m_StrokeDirtyW, m_StrokeDirtyH);
        }

        // ALL FOUR or none. A command holding a complete pair 0 and a broken pair 1
        // would restore splatmap 0 alone, and since the kernel re-normalises across
        // all eight channels that leaves the weights not summing to 1 — a partial
        // undo that silently changes every other layer's contribution at those
        // texels, which is worse than having no undo entry at all.
        if (before0 == TerrainTextureUndoStack::kInvalidSnapshot ||
            after0 == TerrainTextureUndoStack::kInvalidSnapshot ||
            (needsSplat1 && (before1 == TerrainTextureUndoStack::kInvalidSnapshot ||
                             after1 == TerrainTextureUndoStack::kInvalidSnapshot)))
        {
            stack.Release(before0);
            stack.Release(after0);
            stack.Release(before1);
            stack.Release(after1);
            OLO_CORE_WARN("TerrainEditorPanel: could not snapshot the paint stroke — no undo entry recorded");
            return;
        }

        m_CommandHistory->PushAlreadyExecuted(
            std::make_unique<TerrainGPUPaintCommand>(m_StrokeMaterial, m_UndoStack,
                                                     before0, after0, before1, after1));
    }

    void TerrainEditorPanel::UpdateContinuousErosion()
    {
        // Erosion and the brush modes are mutually exclusive (one m_EditMode), which
        // is what lets this reuse m_StrokePreHeight as its pre-state parking slot
        // rather than carrying a second full-map copy.
        const bool wantRun = m_ErosionContinuous && m_EditMode == TerrainEditMode::Erosion &&
                             m_Context && m_Erosion.IsReady();

        if (!wantRun)
        {
            if (m_ErosionSessionActive)
            {
                EndContinuousErosionSession();
            }
            return;
        }

        Ref<TerrainData> data = m_ErosionTerrainData;
        if (!m_ErosionSessionActive)
        {
            // Bind the session to the first terrain with a GPU heightmap and hold it
            // for the whole session, so toggling the checkbox off restores the same
            // terrain the iterations went into even if the selection changed.
            // Same predicate the Apply-Erosion button uses below, so the checkbox and
            // the button cannot target different entities in a multi-terrain scene.
            auto terrainView = m_Context->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
            for (auto entity : terrainView)
            {
                auto& terrain = terrainView.get<TerrainComponent>(entity);
                if (!terrain.m_TerrainData || !terrain.m_ChunkManager)
                    continue;
                if (!terrain.m_TerrainData->GetGPUHeightmap())
                    continue;

                m_ErosionTerrainData = terrain.m_TerrainData;
                m_ErosionChunkManager = terrain.m_ChunkManager;
                m_ErosionEntity = entity;
                m_ErosionWorldSizeX = terrain.m_WorldSizeX;
                m_ErosionWorldSizeZ = terrain.m_WorldSizeZ;
                m_ErosionHeightScale = terrain.m_HeightScale;
                data = terrain.m_TerrainData;
                break;
            }

            if (!data)
            {
                return;
            }

            TerrainTextureUndoStack::EnsureFullCopy(m_StrokePreHeight, data->GetGPUHeightmap());
            m_ErosionSessionActive = true;
        }

        if (!data)
        {
            return;
        }

        // Dispatch only. No readback, no chunk rebuild, no collision sync per frame:
        // with GPU-driven LOD the surface is drawn from the heightmap texture, so the
        // convergence is visible immediately, and the CPU-side consumers are brought
        // up to date once when the session ends.
        m_Erosion.ApplyIterations(*data, m_ErosionSettings, m_ErosionIterationsPerFrame);
    }

    void TerrainEditorPanel::EndContinuousErosionSession()
    {
        m_ErosionSessionActive = false;

        Ref<TerrainData> data = m_ErosionTerrainData;
        m_ErosionTerrainData = nullptr;
        Ref<TerrainChunkManager> chunkManager = m_ErosionChunkManager;
        m_ErosionChunkManager = nullptr;
        const entt::entity erosionEntity = m_ErosionEntity;
        m_ErosionEntity = entt::null;

        if (!data)
        {
            return;
        }

        SettleErosionEdit(data, chunkManager, erosionEntity, m_ErosionWorldSizeX, m_ErosionWorldSizeZ,
                          m_ErosionHeightScale, m_StrokePreHeight);
    }

    void TerrainEditorPanel::SettleErosionEdit(const Ref<TerrainData>& data,
                                               Ref<TerrainChunkManager> chunkManager,
                                               entt::entity entity,
                                               f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                                               const Ref<Texture2D>& preImage)
    {
        const u32 resolution = data->GetResolution();
        if (resolution == 0)
        {
            return;
        }

        // Erosion rewrites the whole field, so the undo region is the whole map. The
        // ring's byte budget is what keeps a long erosion session from being an
        // unbounded VRAM cost — old sessions age out.
        if (m_CommandHistory && preImage)
        {
            TerrainTextureUndoStack& stack = EnsureUndoStack();
            const auto before = stack.Capture(preImage, 0, 0, resolution, resolution);
            const auto after = stack.Capture(data->GetGPUHeightmap(), 0, 0, resolution, resolution);

            if (before != TerrainTextureUndoStack::kInvalidSnapshot &&
                after != TerrainTextureUndoStack::kInvalidSnapshot)
            {
                Entity ent = (m_Context && entity != entt::null) ? Entity{ entity, m_Context.get() } : Entity{};
                const UUID uuid = ent ? ent.GetUUID() : UUID(0);
                m_CommandHistory->PushAlreadyExecuted(
                    std::make_unique<TerrainGPUSculptCommand>(
                        data, chunkManager, m_UndoStack,
                        worldSizeX, worldSizeZ, heightScale,
                        0, 0, resolution, resolution, before, after,
                        WeakRef<Scene>(m_Context), uuid));
            }
            else
            {
                stack.Release(before);
                stack.Release(after);
                OLO_CORE_WARN("TerrainEditorPanel: could not snapshot the erosion pass — no undo entry recorded");
            }
        }

        // The CPU-side consumers catch up here, once — this is the single
        // TerrainData::SyncFromGPU the whole session pays for, however many
        // iterations ran. NOTE there is deliberately no UploadToGPU: the GPU copy is
        // the newer one, and pushing the mirror back over it would undo the erosion.
        if (chunkManager && chunkManager->IsBuilt())
        {
            chunkManager->GenerateAllChunks(*data, worldSizeX, worldSizeZ, heightScale);
        }

        if (m_Context && entity != entt::null)
        {
            Entity terrainEnt{ entity, m_Context.get() };
            m_Context->UpdateTerrainCollisionAfterEdit(terrainEnt, 0, 0, resolution, resolution);
        }
    }

    void TerrainEditorPanel::OnUpdate(f32 deltaTime, const glm::vec3& hitPos, bool hasHit, bool mouseDown)
    {
        m_BrushWorldPos = hitPos;
        m_HasBrushHit = hasHit;

        // Stroke end detection: was painting but mouse released
        if (m_StrokeActive && !mouseDown)
        {
            // Finalize stroke and push undo command
            if (m_StrokeUsesGPU)
            {
                // GPU path: undo is a pair of texture-region snapshots rather than a
                // pair of std::vector regions, so the commit is entirely different
                // code — see CommitGPUSculptStroke / CommitGPUPaintStroke. Guarded on
                // the dirty rect for the same reason as the CPU path: a press with no
                // texel actually touched is not an undo step.
                if (m_StrokeDirtyW > 0 && m_StrokeDirtyH > 0)
                {
                    if (m_EditMode == TerrainEditMode::Sculpt)
                    {
                        CommitGPUSculptStroke();
                    }
                    else if (m_EditMode == TerrainEditMode::Paint)
                    {
                        CommitGPUPaintStroke();
                    }
                    else
                    {
                        // No additional handling required — Voxel commits in
                        // OnVoxelUpdate, and the remaining modes take no stroke.
                    }
                }

                // Settle work deferred off the per-frame path (issue #716). Both of
                // these read the CPU mirror, so this is where the stroke's ONE
                // readback happens — not one per drag frame.
                if (m_EditMode == TerrainEditMode::Sculpt && m_StrokeTerrainData &&
                    m_StrokeDirtyW > 0 && m_StrokeDirtyH > 0)
                {
                    if (m_StrokeChunkManager)
                    {
                        TerrainBrush::DirtyRegion dirty{ m_StrokeDirtyX, m_StrokeDirtyY,
                                                         m_StrokeDirtyW, m_StrokeDirtyH };
                        TerrainBrush::RebuildDirtyChunks(*m_StrokeChunkManager, *m_StrokeTerrainData, dirty,
                                                         m_StrokeWorldSizeX, m_StrokeWorldSizeZ,
                                                         m_StrokeHeightScale);
                    }

                    if (m_Context && m_StrokeEntity != entt::null)
                    {
                        Entity strokeEnt{ m_StrokeEntity, m_Context.get() };
                        if (strokeEnt)
                        {
                            m_Context->UpdateTerrainCollisionAfterEdit(
                                strokeEnt, m_StrokeDirtyX, m_StrokeDirtyY, m_StrokeDirtyW, m_StrokeDirtyH);
                        }
                    }
                }
            }
            else if (m_CommandHistory && m_StrokeDirtyW > 0 && m_StrokeDirtyH > 0)
            {
                if (m_EditMode == TerrainEditMode::Sculpt && m_StrokeTerrainData)
                {
                    // Extract old heights for the dirty region from the full snapshot
                    u32 resolution = m_StrokeTerrainData->GetResolution();
                    std::vector<f32> oldRegion(m_StrokeDirtyW * m_StrokeDirtyH);
                    for (u32 row = 0; row < m_StrokeDirtyH; ++row)
                    {
                        u32 srcIdx = (m_StrokeDirtyY + row) * resolution + m_StrokeDirtyX;
                        u32 dstIdx = row * m_StrokeDirtyW;
                        std::memcpy(&oldRegion[dstIdx], &m_StrokeOldHeights[srcIdx], m_StrokeDirtyW * sizeof(f32));
                    }

                    // Capture new heights from the dirty region
                    const auto& fullData = m_StrokeTerrainData->GetHeightData();
                    std::vector<f32> newHeights(m_StrokeDirtyW * m_StrokeDirtyH);
                    for (u32 row = 0; row < m_StrokeDirtyH; ++row)
                    {
                        u32 srcIdx = (m_StrokeDirtyY + row) * resolution + m_StrokeDirtyX;
                        u32 dstIdx = row * m_StrokeDirtyW;
                        std::memcpy(&newHeights[dstIdx], &fullData[srcIdx], m_StrokeDirtyW * sizeof(f32));
                    }

                    // Resolve the stroke's terrain entity once: the undo command uses it to
                    // refresh collision on redo/undo (held weakly, issue #469 review), and the
                    // stroke-settle call below covers the initial live-applied stroke.
                    Entity strokeEnt = (m_Context && m_StrokeEntity != entt::null)
                                           ? Entity{ m_StrokeEntity, m_Context.get() }
                                           : Entity{};
                    const UUID strokeUUID = strokeEnt ? strokeEnt.GetUUID() : UUID(0);

                    m_CommandHistory->PushAlreadyExecuted(
                        std::make_unique<TerrainSculptCommand>(
                            m_StrokeTerrainData, m_StrokeChunkManager,
                            m_StrokeWorldSizeX, m_StrokeWorldSizeZ, m_StrokeHeightScale,
                            m_StrokeDirtyX, m_StrokeDirtyY, m_StrokeDirtyW, m_StrokeDirtyH,
                            std::move(oldRegion), std::move(newHeights),
                            WeakRef<Scene>(m_Context), strokeUUID));

                    // Sync collision once at stroke settle so a body dropped on the sculpted
                    // region rests on the NEW surface (issue #469). No-op unless physics is
                    // running (Play/Simulate); debounced here rather than per drag frame.
                    if (strokeEnt)
                    {
                        m_Context->UpdateTerrainCollisionAfterEdit(
                            strokeEnt, m_StrokeDirtyX, m_StrokeDirtyY, m_StrokeDirtyW, m_StrokeDirtyH);
                    }
                }
                else if (m_EditMode == TerrainEditMode::Paint && m_StrokeMaterial)
                {
                    u32 resolution = m_StrokeMaterial->GetSplatmapResolution();
                    constexpr u32 channels = 4;

                    // Extract old splatmap0 region from full snapshot
                    std::vector<u8> oldRegion0(m_StrokeDirtyW * m_StrokeDirtyH * channels);
                    for (u32 row = 0; row < m_StrokeDirtyH; ++row)
                    {
                        u32 srcIdx = ((m_StrokeDirtyY + row) * resolution + m_StrokeDirtyX) * channels;
                        u32 dstIdx = row * m_StrokeDirtyW * channels;
                        std::memcpy(&oldRegion0[dstIdx], &m_StrokeOldSplatmap0[srcIdx], m_StrokeDirtyW * channels);
                    }

                    // Capture new splatmap data
                    auto& splatmap0 = m_StrokeMaterial->GetSplatmapData(0);
                    std::vector<u8> newSplatmap0(m_StrokeDirtyW * m_StrokeDirtyH * channels);
                    for (u32 row = 0; row < m_StrokeDirtyH; ++row)
                    {
                        u32 srcIdx = ((m_StrokeDirtyY + row) * resolution + m_StrokeDirtyX) * channels;
                        u32 dstIdx = row * m_StrokeDirtyW * channels;
                        std::memcpy(&newSplatmap0[dstIdx], &splatmap0[srcIdx], m_StrokeDirtyW * channels);
                    }

                    // If using second splatmap (>4 layers), create a compound command
                    if (m_StrokeMaterial->GetLayerCount() > 4 && !m_StrokeOldSplatmap1.empty())
                    {
                        auto compound = std::make_unique<CompoundCommand>("Terrain Paint");

                        compound->Add(std::make_unique<TerrainPaintCommand>(
                            m_StrokeMaterial, 0,
                            m_StrokeDirtyX, m_StrokeDirtyY, m_StrokeDirtyW, m_StrokeDirtyH,
                            std::move(oldRegion0), std::move(newSplatmap0)));

                        std::vector<u8> oldRegion1(m_StrokeDirtyW * m_StrokeDirtyH * channels);
                        for (u32 row = 0; row < m_StrokeDirtyH; ++row)
                        {
                            u32 srcIdx = ((m_StrokeDirtyY + row) * resolution + m_StrokeDirtyX) * channels;
                            u32 dstIdx = row * m_StrokeDirtyW * channels;
                            std::memcpy(&oldRegion1[dstIdx], &m_StrokeOldSplatmap1[srcIdx], m_StrokeDirtyW * channels);
                        }

                        auto& splatmap1 = m_StrokeMaterial->GetSplatmapData(1);
                        std::vector<u8> newSplatmap1(m_StrokeDirtyW * m_StrokeDirtyH * channels);
                        for (u32 row = 0; row < m_StrokeDirtyH; ++row)
                        {
                            u32 srcIdx = ((m_StrokeDirtyY + row) * resolution + m_StrokeDirtyX) * channels;
                            u32 dstIdx = row * m_StrokeDirtyW * channels;
                            std::memcpy(&newSplatmap1[dstIdx], &splatmap1[srcIdx], m_StrokeDirtyW * channels);
                        }

                        compound->Add(std::make_unique<TerrainPaintCommand>(
                            m_StrokeMaterial, 1,
                            m_StrokeDirtyX, m_StrokeDirtyY, m_StrokeDirtyW, m_StrokeDirtyH,
                            std::move(oldRegion1), std::move(newSplatmap1)));

                        m_CommandHistory->PushAlreadyExecuted(std::move(compound));
                    }
                    else
                    {
                        m_CommandHistory->PushAlreadyExecuted(
                            std::make_unique<TerrainPaintCommand>(
                                m_StrokeMaterial, 0,
                                m_StrokeDirtyX, m_StrokeDirtyY, m_StrokeDirtyW, m_StrokeDirtyH,
                                std::move(oldRegion0), std::move(newSplatmap0)));
                    }
                }
                // No Voxel case here: EditorLayer routes Voxel mode to
                // OnVoxelUpdate instead of this function, which is where the
                // stroke is committed. The resets below still clear the voxel
                // stroke state, so a mode switch mid-stroke leaves nothing behind.
            }

            m_StrokeActive = false;
            m_StrokeUsesGPU = false;
            m_StrokeTargetHeight = 0.0f;
            m_StrokeDirtyX = m_StrokeDirtyY = m_StrokeDirtyW = m_StrokeDirtyH = 0;
            m_StrokeOldHeights.clear();
            m_StrokeOldSplatmap0.clear();
            m_StrokeOldSplatmap1.clear();
            m_StrokeTerrainData = nullptr;
            m_StrokeChunkManager = nullptr;
            m_StrokeMaterial = nullptr;
            m_StrokeVoxels = nullptr;
            m_VoxelStroke = {};
            m_StrokeEntity = entt::null;
        }

        if (!hasHit || !mouseDown || m_EditMode == TerrainEditMode::None || !m_Context)
            return;

        auto terrainView = m_Context->GetAllEntitiesWith<TransformComponent, TerrainComponent>();

        for (auto entity : terrainView)
        {
            auto& terrain = terrainView.get<TerrainComponent>(entity);

            if (!terrain.m_TerrainData || !terrain.m_ChunkManager || !terrain.m_ChunkManager->IsBuilt())
                continue;

            if (m_EditMode == TerrainEditMode::Sculpt)
            {
                // Three conditions, and the third is the interesting one. The GPU
                // sculpt path defers the chunk-mesh rebuild to stroke settle, because
                // that rebuild reads the CPU mirror and doing it per drag frame would
                // reinstate exactly the per-operation readback this issue removes. That
                // is only acceptable while the terrain is DRAWN from the heightmap
                // texture — i.e. on the GPU-driven LOD path, where the stroke is
                // visible immediately and the meshes are only wanted by the CPU
                // fallback. With GPU LOD off the user would see nothing until release,
                // so that configuration keeps the CPU brush: no better, but no worse
                // than before, and never a silent regression.
                const bool useGPU = m_GPUBrush.IsSculptReady() &&
                                    terrain.m_TerrainData->GetGPUHeightmap() != nullptr &&
                                    terrain.m_TessellationEnabled &&
                                    TerrainChunkManager::IsGpuDrivenLODEnabled();

                // Stroke start. No longer gated on m_CommandHistory: an editor with no
                // undo stack must still set up the stroke (the GPU path reads
                // m_StrokeUsesGPU and m_StrokeTargetHeight every frame), it just does
                // not get an undo entry — the same trade the voxel path documents.
                if (!m_StrokeActive)
                {
                    m_StrokeActive = true;
                    m_StrokeUsesGPU = useGPU;
                    m_StrokeTerrainData = terrain.m_TerrainData;
                    m_StrokeChunkManager = terrain.m_ChunkManager;
                    m_StrokeEntity = entity;
                    m_StrokeWorldSizeX = terrain.m_WorldSizeX;
                    m_StrokeWorldSizeZ = terrain.m_WorldSizeZ;
                    m_StrokeHeightScale = terrain.m_HeightScale;
                    m_StrokeDirtyX = m_StrokeDirtyY = m_StrokeDirtyW = m_StrokeDirtyH = 0;
                    m_StrokeTargetHeight = 0.0f;

                    if (useGPU)
                    {
                        // Flatten and Level converge on the height under the press.
                        // Sampled ONCE, here: this is a CPU height query, so on the GPU
                        // path it costs one TerrainData::SyncFromGPU — acceptable per
                        // stroke, not acceptable per frame, which is why the kernel
                        // takes it as a uniform instead of re-reading it.
                        if (m_SculptSettings.Tool == TerrainBrushTool::Flatten ||
                            m_SculptSettings.Tool == TerrainBrushTool::Level)
                        {
                            m_StrokeTargetHeight = terrain.m_TerrainData->GetHeightAt(
                                hitPos.x / terrain.m_WorldSizeX, hitPos.z / terrain.m_WorldSizeZ);
                        }

                        TerrainTextureUndoStack::EnsureFullCopy(m_StrokePreHeight,
                                                                terrain.m_TerrainData->GetGPUHeightmap());
                    }
                    else
                    {
                        // Full copy of heightmap for old-state extraction at stroke end
                        m_StrokeOldHeights = terrain.m_TerrainData->GetHeightData();
                    }
                }

                // `useGPU` for THIS terrain, not the stroke's flag: in a multi-terrain
                // scene m_StrokeUsesGPU was decided by whichever terrain started the
                // stroke, and a sibling with tessellation off must not be dispatched
                // to (nor a sibling with it on be brushed on the CPU). m_StrokeUsesGPU
                // stays authoritative for the settle-time commit, which is per stroke.
                auto dirty = useGPU
                                 ? m_GPUBrush.ApplySculpt(
                                       *terrain.m_TerrainData, m_SculptSettings, hitPos,
                                       terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale,
                                       deltaTime, m_StrokeTargetHeight)
                                 : TerrainBrush::Apply(
                                       *terrain.m_TerrainData, m_SculptSettings, hitPos,
                                       terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale,
                                       deltaTime);

                if (dirty.Width > 0 && dirty.Height > 0)
                {
                    // The GPU brush wrote the texture directly — there is nothing to
                    // upload, and uploading the CPU mirror here would overwrite it.
                    if (!useGPU)
                    {
                        terrain.m_TerrainData->UploadRegionToGPU(dirty.X, dirty.Y, dirty.Width, dirty.Height);
                    }

                    // Deferred to stroke settle on the GPU path — see the useGPU
                    // comment above for why that is safe there and not otherwise.
                    if (!useGPU)
                    {
                        TerrainBrush::RebuildDirtyChunks(
                            *terrain.m_ChunkManager, *terrain.m_TerrainData,
                            dirty, terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);
                    }

                    // Expand stroke dirty region
                    if (m_StrokeDirtyW == 0)
                    {
                        // First dirty region — snapshot the old heights before the apply above changed them
                        // Note: the apply already happened, so for the very first frame we need the old data.
                        // We snapshotted the full heightmap on stroke start, so extract from there.
                        m_StrokeDirtyX = dirty.X;
                        m_StrokeDirtyY = dirty.Y;
                        m_StrokeDirtyW = dirty.Width;
                        m_StrokeDirtyH = dirty.Height;
                    }
                    else
                    {
                        // Expand bounding rect
                        u32 minX = std::min(m_StrokeDirtyX, dirty.X);
                        u32 minY = std::min(m_StrokeDirtyY, dirty.Y);
                        u32 maxX = std::max(m_StrokeDirtyX + m_StrokeDirtyW, dirty.X + dirty.Width);
                        u32 maxY = std::max(m_StrokeDirtyY + m_StrokeDirtyH, dirty.Y + dirty.Height);
                        m_StrokeDirtyX = minX;
                        m_StrokeDirtyY = minY;
                        m_StrokeDirtyW = maxX - minX;
                        m_StrokeDirtyH = maxY - minY;
                    }
                }
            }
            else if (m_EditMode == TerrainEditMode::Paint)
            {
                if (!terrain.m_Material)
                    continue;

                // Initialize CPU splatmaps if not done yet
                if (!terrain.m_Material->HasCPUSplatmaps())
                {
                    u32 splatRes = terrain.m_TerrainData->GetResolution();
                    terrain.m_Material->InitializeCPUSplatmaps(splatRes);
                }

                const bool usePaintGPU = m_GPUBrush.IsPaintReady() &&
                                         terrain.m_Material->GetSplatmap(0) != nullptr &&
                                         terrain.m_Material->GetSplatmap(1) != nullptr;

                // Snapshot splatmap on stroke start
                if (!m_StrokeActive)
                {
                    m_StrokeActive = true;
                    m_StrokeUsesGPU = usePaintGPU;
                    m_StrokeMaterial = terrain.m_Material;
                    m_StrokeWorldSizeX = terrain.m_WorldSizeX;
                    m_StrokeWorldSizeZ = terrain.m_WorldSizeZ;
                    m_StrokeDirtyX = m_StrokeDirtyY = m_StrokeDirtyW = m_StrokeDirtyH = 0;

                    if (usePaintGPU)
                    {
                        TerrainTextureUndoStack::EnsureFullCopy(m_StrokePreSplat0,
                                                                terrain.m_Material->GetSplatmap(0));
                        if (terrain.m_Material->GetLayerCount() > 4)
                        {
                            TerrainTextureUndoStack::EnsureFullCopy(m_StrokePreSplat1,
                                                                    terrain.m_Material->GetSplatmap(1));
                        }
                    }
                    else
                    {
                        // Full copy of splatmap(s) for old-state extraction at stroke end
                        m_StrokeOldSplatmap0 = terrain.m_Material->GetSplatmapData(0);
                        if (terrain.m_Material->GetLayerCount() > 4)
                        {
                            m_StrokeOldSplatmap1 = terrain.m_Material->GetSplatmapData(1);
                        }
                    }
                }

                auto dirty = m_StrokeUsesGPU
                                 ? m_GPUBrush.ApplyPaint(*terrain.m_Material, m_PaintSettings, hitPos,
                                                         terrain.m_WorldSizeX, terrain.m_WorldSizeZ, deltaTime)
                                 : TerrainPaintBrush::Apply(*terrain.m_Material, m_PaintSettings, hitPos,
                                                            terrain.m_WorldSizeX, terrain.m_WorldSizeZ,
                                                            deltaTime);

                if (dirty.Width > 0 && dirty.Height > 0)
                {
                    // Upload both splatmaps (the normalization may affect both). Only on
                    // the CPU path — the kernel wrote the textures itself.
                    if (!m_StrokeUsesGPU)
                    {
                        terrain.m_Material->UploadSplatmapRegion(0, dirty.X, dirty.Y, dirty.Width, dirty.Height);
                        if (terrain.m_Material->GetLayerCount() > 4)
                        {
                            terrain.m_Material->UploadSplatmapRegion(1, dirty.X, dirty.Y, dirty.Width, dirty.Height);
                        }
                    }

                    // Expand stroke dirty region
                    if (m_StrokeDirtyW == 0)
                    {
                        m_StrokeDirtyX = dirty.X;
                        m_StrokeDirtyY = dirty.Y;
                        m_StrokeDirtyW = dirty.Width;
                        m_StrokeDirtyH = dirty.Height;
                    }
                    else
                    {
                        u32 minX = std::min(m_StrokeDirtyX, dirty.X);
                        u32 minY = std::min(m_StrokeDirtyY, dirty.Y);
                        u32 maxX = std::max(m_StrokeDirtyX + m_StrokeDirtyW, dirty.X + dirty.Width);
                        u32 maxY = std::max(m_StrokeDirtyY + m_StrokeDirtyH, dirty.Y + dirty.Height);
                        m_StrokeDirtyX = minX;
                        m_StrokeDirtyY = minY;
                        m_StrokeDirtyW = maxX - minX;
                        m_StrokeDirtyH = maxY - minY;
                    }
                }
            }
            else
            {
                // No additional handling required.
            }
        }
    }

    void TerrainEditorPanel::OnVoxelUpdate(Ref<VoxelOverride> voxels, const VoxelRayHit& hit, bool mouseDown)
    {
        if (m_EditMode != TerrainEditMode::Voxel)
            return;

        m_HasBrushHit = hit.Hit;
        if (hit.Hit)
            m_BrushWorldPos = hit.Point;

        if (m_StrokeActive && !mouseDown)
        {
            if (m_CommandHistory && m_StrokeVoxels && !m_VoxelStroke.Empty())
                m_CommandHistory->PushAlreadyExecuted(std::make_unique<VoxelEditCommand>(m_StrokeVoxels, std::move(m_VoxelStroke)));
            m_StrokeActive = false;
            m_StrokeVoxels = nullptr;
            m_VoxelStroke = {};
            return;
        }
        if (!voxels || !hit.Hit || !mouseDown)
            return;

        // Deliberately not gated on m_CommandHistory: an editor without an undo
        // stack can still edit voxels, it just cannot undo them. The push above
        // is the conditional part, so losing the history costs the undo entry
        // rather than silently swallowing the brush.
        if (!m_StrokeActive)
        {
            m_StrokeActive = true;
            m_StrokeVoxels = voxels;
            m_VoxelStroke = {};
        }

        VoxelEditStroke frame = ApplyVoxelBrush(*voxels, hit, m_VoxelSettings);
        for (auto& [coord, snapshot] : frame.Before)
            m_VoxelStroke.Before.try_emplace(coord, std::move(snapshot));
        for (auto& [coord, snapshot] : frame.After)
            m_VoxelStroke.After.insert_or_assign(coord, std::move(snapshot));
    }

    f32 TerrainEditorPanel::GetBrushRadius() const
    {
        switch (m_EditMode)
        {
            case TerrainEditMode::Sculpt:
                return m_SculptSettings.Radius;
            case TerrainEditMode::Paint:
                return m_PaintSettings.Radius;
            case TerrainEditMode::Voxel:
                return m_VoxelSettings.Radius;
            default:
                return 0.0f;
        }
    }

    void TerrainEditorPanel::DrawVoxelUI()
    {
        static constexpr const char* operationNames[] = { "Place", "Carve", "Fill", "Paint" };
        i32 operation = static_cast<i32>(m_VoxelSettings.Operation);
        if (ImGui::Combo("Operation", &operation, operationNames, IM_ARRAYSIZE(operationNames)))
            m_VoxelSettings.Operation = static_cast<VoxelBrushOperation>(operation);
        ImGui::DragFloat("Radius", &m_VoxelSettings.Radius, 0.1f, 0.0f, 64.0f, "%.2f");
        i32 material = m_VoxelSettings.Material;
        if (ImGui::DragInt("Material", &material, 1.0f, 0, 255))
            m_VoxelSettings.Material = static_cast<u8>(std::clamp(material, 0, 255));
        ImGui::TextDisabled("Click a voxel surface to edit its exact grid cell. A stroke is one undo step.");
    }

    void TerrainEditorPanel::DrawErosionUI()
    {
        ImGui::Text("Hydraulic Erosion");
        ImGui::TextWrapped("Simulates water droplets flowing downhill, eroding and depositing sediment to create realistic terrain features.");

        ImGui::Separator();
        ImGui::Text("Simulation");

        if (int dropletCount = static_cast<int>(m_ErosionSettings.DropletCount); ImGui::DragInt("Droplets", &dropletCount, 1000, 1000, 500000))
            m_ErosionSettings.DropletCount = static_cast<u32>(std::max(1000, dropletCount));
        ImGui::SetItemTooltip("Number of water droplets per iteration");

        if (int maxSteps = static_cast<int>(m_ErosionSettings.MaxDropletSteps); ImGui::DragInt("Max Steps", &maxSteps, 1, 16, 256))
            m_ErosionSettings.MaxDropletSteps = static_cast<u32>(std::max(16, maxSteps));

        if (int iterations = static_cast<int>(m_ErosionIterations); ImGui::DragInt("Iterations", &iterations, 1, 1, 50))
            m_ErosionIterations = static_cast<u32>(std::max(1, iterations));

        ImGui::Separator();
        ImGui::Text("Parameters");

        ImGui::DragFloat("Inertia", &m_ErosionSettings.Inertia, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("How much the droplet's previous direction influences its new direction");

        ImGui::DragFloat("Sediment Capacity", &m_ErosionSettings.SedimentCapacity, 0.1f, 0.1f, 20.0f, "%.1f");
        ImGui::DragFloat("Min Capacity", &m_ErosionSettings.MinSedimentCapacity, 0.001f, 0.001f, 0.1f, "%.3f");
        ImGui::DragFloat("Deposit Speed", &m_ErosionSettings.DepositSpeed, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::DragFloat("Erode Speed", &m_ErosionSettings.ErodeSpeed, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::DragFloat("Evaporation", &m_ErosionSettings.EvaporateSpeed, 0.001f, 0.0f, 0.1f, "%.3f");
        ImGui::DragFloat("Gravity", &m_ErosionSettings.Gravity, 0.1f, 0.5f, 20.0f, "%.1f");

        if (int erosionRadius = m_ErosionSettings.ErosionRadius; ImGui::DragInt("Erosion Radius", &erosionRadius, 1, 1, 8))
            m_ErosionSettings.ErosionRadius = std::max(1, erosionRadius);
        ImGui::SetItemTooltip("Brush radius for erosion/deposition in texels");

        ImGui::Separator();

        if (!m_Erosion.IsReady())
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Erosion shader not loaded!");
        }
        else
        {
            auto terrainView = m_Context->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
            bool hasTerrain = false;

            for (auto entity : terrainView)
            {
                auto& terrain = terrainView.get<TerrainComponent>(entity);
                if (!terrain.m_TerrainData || !terrain.m_ChunkManager)
                    continue;

                hasTerrain = true;

                // Continuous mode is the interactive one (issue #716): every frame
                // runs Iterations/Frame more droplets into the same GPU heightmap and
                // the surface visibly converges while the slider moves. It is only
                // affordable because Apply() no longer reads the map back — with the
                // old per-iteration GetData this was a whole-map GPU->CPU stall per
                // frame. Undo is grouped per SESSION: one entry when the box is
                // unticked, not one per frame.
                ImGui::Checkbox("Continuous", &m_ErosionContinuous);
                ImGui::SetItemTooltip("Erode every frame so the surface converges live. "
                                      "Unticking commits one undo entry for the whole session.");

                if (int perFrame = static_cast<int>(m_ErosionIterationsPerFrame);
                    ImGui::SliderInt("Iterations/Frame", &perFrame, 1, 16))
                {
                    m_ErosionIterationsPerFrame = static_cast<u32>(std::max(1, perFrame));
                }
                ImGui::SetItemTooltip("Erosion rate while Continuous is on — drag this to watch it converge");

                if (m_ErosionContinuous)
                {
                    ImGui::TextDisabled("Eroding... (untick to commit)");
                }

                ImGui::Separator();

                ImGui::BeginDisabled(m_ErosionContinuous);
                if (ImGui::Button("Apply Erosion", ImVec2(-1, 30)))
                {
                    // Snapshot before the dispatches, so the undo entry the settle
                    // records has something to go back to.
                    Ref<Texture2D> preImage;
                    TerrainTextureUndoStack::EnsureFullCopy(preImage,
                                                            terrain.m_TerrainData->GetGPUHeightmap());

                    m_Erosion.ApplyIterations(*terrain.m_TerrainData, m_ErosionSettings, m_ErosionIterations);

                    // NO UploadToGPU here. It used to re-push the CPU heightmap after
                    // the readback kept the two in step; now the GPU copy is the newer
                    // one and pushing the mirror over it would silently undo the whole
                    // erosion pass. The settle below syncs in the other direction.
                    SettleErosionEdit(terrain.m_TerrainData, terrain.m_ChunkManager, entity,
                                      terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale,
                                      preImage);
                }
                ImGui::EndDisabled();
                break;
            }

            if (!hasTerrain)
            {
                ImGui::TextDisabled("No terrain with heightmap in scene");
            }
        }
    }

    f32 TerrainEditorPanel::GetBrushFalloff() const
    {
        switch (m_EditMode)
        {
            case TerrainEditMode::Sculpt:
                return m_SculptSettings.Falloff;
            case TerrainEditMode::Paint:
                return m_PaintSettings.Falloff;
            case TerrainEditMode::Voxel:
                return 1.0f;
            default:
                return 0.5f;
        }
    }
} // namespace OloEngine
