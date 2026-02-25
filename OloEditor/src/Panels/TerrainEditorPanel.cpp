#include "TerrainEditorPanel.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/Editor/TerrainErosion.h"

#include <imgui.h>

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
        bool hasTerrain = false;
        auto terrainView = m_Context->GetAllEntitiesWith<TransformComponent, TerrainComponent>();
        for ([[maybe_unused]] auto entity : terrainView)
        {
            hasTerrain = true;
            break;
        }

        if (!hasTerrain)
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
        if (ImGui::RadioButton("Sculpt", m_EditMode == TerrainEditMode::Sculpt))
            m_EditMode = TerrainEditMode::Sculpt;
        ImGui::SameLine();
        if (ImGui::RadioButton("Paint", m_EditMode == TerrainEditMode::Paint))
            m_EditMode = TerrainEditMode::Paint;
        ImGui::SameLine();
        if (ImGui::RadioButton("Erosion", m_EditMode == TerrainEditMode::Erosion))
            m_EditMode = TerrainEditMode::Erosion;

        ImGui::Separator();

        switch (m_EditMode)
        {
            case TerrainEditMode::Sculpt:
                DrawSculptUI();
                break;
            case TerrainEditMode::Paint:
                DrawPaintUI();
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

    void TerrainEditorPanel::DrawSculptUI()
    {
        ImGui::Text("Sculpt Tool");

        int currentTool = static_cast<int>(m_SculptSettings.Tool);
        if (ImGui::Combo("Tool", &currentTool, s_SculptToolNames, IM_ARRAYSIZE(s_SculptToolNames)))
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

        int targetLayer = static_cast<int>(m_PaintSettings.TargetLayer);
        if (ImGui::SliderInt("Target Layer", &targetLayer, 0, static_cast<int>(maxLayers - 1)))
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

    void TerrainEditorPanel::OnUpdate(f32 deltaTime, const glm::vec3& hitPos, bool hasHit, bool mouseDown)
    {
        m_BrushWorldPos = hitPos;
        m_HasBrushHit = hasHit;

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
                auto dirty = TerrainBrush::Apply(
                    *terrain.m_TerrainData,
                    m_SculptSettings,
                    hitPos,
                    terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale,
                    deltaTime);

                if (dirty.Width > 0 && dirty.Height > 0)
                {
                    terrain.m_TerrainData->UploadRegionToGPU(dirty.X, dirty.Y, dirty.Width, dirty.Height);
                    TerrainBrush::RebuildDirtyChunks(
                        *terrain.m_ChunkManager, *terrain.m_TerrainData,
                        dirty, terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);
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

                auto dirty = TerrainPaintBrush::Apply(
                    *terrain.m_Material,
                    m_PaintSettings,
                    hitPos,
                    terrain.m_WorldSizeX, terrain.m_WorldSizeZ,
                    deltaTime);

                if (dirty.Width > 0 && dirty.Height > 0)
                {
                    // Upload both splatmaps (the normalization may affect both)
                    terrain.m_Material->UploadSplatmapRegion(0, dirty.X, dirty.Y, dirty.Width, dirty.Height);
                    if (terrain.m_Material->GetLayerCount() > 4)
                    {
                        terrain.m_Material->UploadSplatmapRegion(1, dirty.X, dirty.Y, dirty.Width, dirty.Height);
                    }
                }
            }
        }
    }

    f32 TerrainEditorPanel::GetBrushRadius() const
    {
        switch (m_EditMode)
        {
            case TerrainEditMode::Sculpt: return m_SculptSettings.Radius;
            case TerrainEditMode::Paint: return m_PaintSettings.Radius;
            default: return 0.0f;
        }
    }

    void TerrainEditorPanel::DrawErosionUI()
    {
        ImGui::Text("Hydraulic Erosion");
        ImGui::TextWrapped("Simulates water droplets flowing downhill, eroding and depositing sediment to create realistic terrain features.");

        ImGui::Separator();
        ImGui::Text("Simulation");

        int dropletCount = static_cast<int>(m_ErosionSettings.DropletCount);
        if (ImGui::DragInt("Droplets", &dropletCount, 1000, 1000, 500000))
            m_ErosionSettings.DropletCount = static_cast<u32>(std::max(1000, dropletCount));
        ImGui::SetItemTooltip("Number of water droplets per iteration");

        int maxSteps = static_cast<int>(m_ErosionSettings.MaxDropletSteps);
        if (ImGui::DragInt("Max Steps", &maxSteps, 1, 16, 256))
            m_ErosionSettings.MaxDropletSteps = static_cast<u32>(std::max(16, maxSteps));

        int iterations = static_cast<int>(m_ErosionIterations);
        if (ImGui::DragInt("Iterations", &iterations, 1, 1, 50))
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

        int erosionRadius = m_ErosionSettings.ErosionRadius;
        if (ImGui::DragInt("Erosion Radius", &erosionRadius, 1, 1, 8))
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

                if (ImGui::Button("Apply Erosion", ImVec2(-1, 30)))
                {
                    m_Erosion.ApplyIterations(*terrain.m_TerrainData, m_ErosionSettings, m_ErosionIterations);

                    // Re-upload full heightmap and rebuild all chunks
                    terrain.m_TerrainData->UploadToGPU();
                    if (terrain.m_ChunkManager->IsBuilt())
                    {
                        terrain.m_ChunkManager->GenerateAllChunks(
                            *terrain.m_TerrainData,
                            terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);
                    }
                }
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
            case TerrainEditMode::Sculpt: return m_SculptSettings.Falloff;
            case TerrainEditMode::Paint: return m_PaintSettings.Falloff;
            default: return 0.5f;
        }
    }
} // namespace OloEngine
