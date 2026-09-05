#include "OloEnginePCH.h"
#include "Panels/TilemapPainterPanel.h"

#include "../UndoRedo/EditorCommand.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Tilemap/Tileset.h"

#include "OloEngine/ImGui/ImGuiLayer.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
    // Interprets ImGui drag-drop payload bytes as a UTF-8 path.
    [[nodiscard]] std::filesystem::path PathFromUtf8Payload(const ImGuiPayload& payload)
    {
        auto const* data = static_cast<char const*>(payload.Data);
        auto const* u8data = reinterpret_cast<char8_t const*>(data);
        size_t len = static_cast<size_t>(payload.DataSize);
        if (len > 0 && data[len - 1] == '\0')
            --len;
        return std::filesystem::path(std::u8string_view(u8data, len));
    }
} // namespace

namespace OloEngine
{
    namespace
    {
        // Undo entry for one completed stroke. The whole layer vector is snapshotted
        // on both sides: a rectangle fill can rewrite thousands of cells, and the
        // vectors are plain ints, so a snapshot pair is both simpler and smaller than
        // a per-cell diff for any stroke worth undoing.
        class TilemapStrokeCommand : public EditorCommand
        {
          public:
            TilemapStrokeCommand(Ref<Scene> scene, UUID entityUUID,
                                 std::vector<TileLayer> preSnapshot,
                                 std::vector<TileLayer> postSnapshot,
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
            void ApplySnapshot(const std::vector<TileLayer>& snapshot)
            {
                if (!m_Scene)
                    return;
                // Resolve by UUID at apply time: entering and leaving Play rebuilds
                // the registry, so a captured Entity would dangle.
                Entity entity = m_Scene->GetEntityByUUID(m_EntityUUID);
                if (!entity || !entity.HasComponent<TilemapComponent>())
                    return;
                entity.GetComponent<TilemapComponent>().Layers = snapshot;
            }

            Ref<Scene> m_Scene;
            UUID m_EntityUUID;
            std::vector<TileLayer> m_PreSnapshot;
            std::vector<TileLayer> m_PostSnapshot;
            std::string m_Description;
        };

        const char* ModeLabel(TilemapPainterPanel::Mode mode)
        {
            switch (mode)
            {
                case TilemapPainterPanel::Mode::Off:
                    return "Off";
                case TilemapPainterPanel::Mode::Paint:
                    return "Paint";
                case TilemapPainterPanel::Mode::Erase:
                    return "Erase";
                case TilemapPainterPanel::Mode::RectFill:
                    return "Rect Fill";
            }
            return "Off";
        }
    } // namespace

    bool TilemapPainterPanel::PickTile(const Ray& ray, u32& outX, u32& outY) const
    {
        if (!m_TargetEntity || !m_TargetEntity.HasComponent<TilemapComponent>())
            return false;

        const auto& tilemap = m_TargetEntity.GetComponent<TilemapComponent>();
        const glm::mat4 transform = m_Context ? m_Context->GetWorldTransform(m_TargetEntity) : glm::mat4(1.0f);

        // The grid lies on the entity's local z = 0 plane. Take the ray into local
        // space and intersect there, which handles a rotated or scaled tilemap
        // without special-casing any of it.
        const glm::mat4 inverse = glm::inverse(transform);
        const glm::vec3 localOrigin = glm::vec3(inverse * glm::vec4(ray.Origin, 1.0f));
        const glm::vec3 localDir = glm::vec3(inverse * glm::vec4(ray.Direction, 0.0f));

        // Bit-exact zero test on purpose: any non-zero z component gives a finite
        // intersection, and comparing against an epsilon here would reject
        // near-edge-on views that still pick a sensible tile.
        if (localDir.z == 0.0f)
            return false;

        const f32 t = -localOrigin.z / localDir.z;
        if (!std::isfinite(t) || t < 0.0f)
            return false;

        const glm::vec3 local = localOrigin + t * localDir;
        if (!std::isfinite(local.x) || !std::isfinite(local.y))
            return false;

        const f32 tileSize = tilemap.TileSize;
        if (local.x < 0.0f || local.y < 0.0f)
            return false;

        const f32 fx = local.x / tileSize;
        const f32 fy = local.y / tileSize;
        if (fx >= static_cast<f32>(tilemap.Width) || fy >= static_cast<f32>(tilemap.Height))
            return false;

        outX = static_cast<u32>(fx);
        outY = static_cast<u32>(fy);
        return true;
    }

    void TilemapPainterPanel::ApplyTile(u32 x, u32 y, u32 value)
    {
        if (!m_TargetEntity || !m_TargetEntity.HasComponent<TilemapComponent>())
            return;
        auto& tilemap = m_TargetEntity.GetComponent<TilemapComponent>();
        if (m_ActiveLayer >= tilemap.Layers.size())
            return;
        if (tilemap.GetTile(m_ActiveLayer, x, y) == value)
            return;
        tilemap.SetTile(m_ActiveLayer, x, y, value);
    }

    void TilemapPainterPanel::ApplyRect(u32 x0, u32 y0, u32 x1, u32 y1, u32 value)
    {
        const u32 minX = std::min(x0, x1);
        const u32 maxX = std::max(x0, x1);
        const u32 minY = std::min(y0, y1);
        const u32 maxY = std::max(y0, y1);
        for (u32 y = minY; y <= maxY; ++y)
        {
            for (u32 x = minX; x <= maxX; ++x)
                ApplyTile(x, y, value);
        }
    }

    void TilemapPainterPanel::BeginStroke()
    {
        if (m_StrokeActive || !m_TargetEntity || !m_TargetEntity.HasComponent<TilemapComponent>())
            return;
        m_StrokePreSnapshot = m_TargetEntity.GetComponent<TilemapComponent>().Layers;
        m_StrokeActive = true;
    }

    void TilemapPainterPanel::EndStroke()
    {
        if (!m_StrokeActive)
            return;
        m_StrokeActive = false;
        m_HasRectAnchor = false;

        if (!m_CommandHistory || !m_TargetEntity || !m_TargetEntity.HasComponent<TilemapComponent>())
        {
            m_StrokePreSnapshot.clear();
            return;
        }

        const auto& tilemap = m_TargetEntity.GetComponent<TilemapComponent>();
        // A stroke that changed nothing (clicked an already-painted cell, dragged
        // outside the grid) must not push an undo entry, or Ctrl+Z starts consuming
        // no-ops before it reaches real work.
        if (m_StrokePreSnapshot == tilemap.Layers)
        {
            m_StrokePreSnapshot.clear();
            return;
        }

        m_CommandHistory->PushAlreadyExecuted(std::make_unique<TilemapStrokeCommand>(
            m_Context, m_TargetEntity.GetUUID(),
            std::move(m_StrokePreSnapshot), tilemap.Layers,
            "Tilemap Paint Stroke"));
        m_StrokePreSnapshot.clear();
    }

    void TilemapPainterPanel::OnUpdate(const Ray& mouseRay, bool hasRay, bool mouseDown)
    {
        if (m_Mode == Mode::Off)
        {
            // Leaving the tool mid-stroke still has to close the undo entry, or the
            // snapshot is silently dropped and the stroke becomes un-undoable.
            if (m_StrokeActive)
                EndStroke();
            m_PrevMouseDown = false;
            return;
        }

        const bool pressed = mouseDown && !m_PrevMouseDown;
        const bool released = !mouseDown && m_PrevMouseDown;
        m_PrevMouseDown = mouseDown;

        u32 tileX = 0;
        u32 tileY = 0;
        const bool hasTile = hasRay && PickTile(mouseRay, tileX, tileY);

        if (m_Mode == Mode::RectFill)
        {
            if (pressed && hasTile)
            {
                m_RectAnchorX = tileX;
                m_RectAnchorY = tileY;
                m_HasRectAnchor = true;
            }
            // The fill lands on release so the drag can be re-aimed; the snapshot is
            // taken at the same moment, which is still "before" every write below.
            if (released && m_HasRectAnchor && hasTile)
            {
                BeginStroke();
                ApplyRect(m_RectAnchorX, m_RectAnchorY, tileX, tileY, m_SelectedTile);
                EndStroke();
            }
            else if (released)
            {
                m_HasRectAnchor = false;
            }
            return;
        }

        if (pressed)
            BeginStroke();

        if (mouseDown && hasTile)
            ApplyTile(tileX, tileY, m_Mode == Mode::Erase ? TilemapComponent::kEmptyTile : m_SelectedTile);

        if (released)
            EndStroke();
    }

    void TilemapPainterPanel::DrawGridSettings()
    {
        auto& tilemap = m_TargetEntity.GetComponent<TilemapComponent>();

        // Resize is applied on a button press rather than live from the drag: a
        // DragInt sweeping 32 -> 8 -> 64 would destroy every column past 8 on the
        // way through, and the undo stack does not cover grid geometry.
        //
        // The pending values are panel members, not statics: a static would carry
        // one tilemap's pending size over to the next entity the author selects
        // and resize the wrong map on the next click.
        if (m_PendingSizeEntity != m_TargetEntity.GetUUID())
        {
            m_PendingSizeEntity = m_TargetEntity.GetUUID();
            m_PendingWidth = static_cast<i32>(tilemap.Width);
            m_PendingHeight = static_cast<i32>(tilemap.Height);
        }

        ImGui::DragInt("Grid Width", &m_PendingWidth, 1.0f, 1, 4096);
        ImGui::DragInt("Grid Height", &m_PendingHeight, 1.0f, 1, 4096);
        if (ImGui::Button("Apply Size"))
        {
            tilemap.Resize(static_cast<u32>(std::clamp(m_PendingWidth, 1, 4096)),
                           static_cast<u32>(std::clamp(m_PendingHeight, 1, 4096)));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(current %ux%u)", tilemap.Width, tilemap.Height);

        ImGui::DragFloat("Tile Size", &tilemap.TileSize, 0.01f, 0.0001f, 10000.0f);
    }

    void TilemapPainterPanel::DrawLayerList()
    {
        auto& tilemap = m_TargetEntity.GetComponent<TilemapComponent>();

        if (ImGui::Button("Add Layer"))
            m_ActiveLayer = tilemap.AddLayer("Layer " + std::to_string(tilemap.Layers.size()));

        if (tilemap.Layers.empty())
        {
            ImGui::TextDisabled("No layers yet - add one to start painting.");
            return;
        }

        m_ActiveLayer = std::min(m_ActiveLayer, tilemap.Layers.size() - 1);

        for (sizet i = 0; i < tilemap.Layers.size(); ++i)
        {
            auto& layer = tilemap.Layers[i];
            ImGui::PushID(static_cast<int>(i));

            if (ImGui::RadioButton("##active", m_ActiveLayer == i))
                m_ActiveLayer = i;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::InputText("##name", &layer.Name);
            ImGui::SameLine();
            ImGui::Checkbox("Visible", &layer.Visible);
            ImGui::SameLine();
            // Toggling Solid needs no rebuild: collision is built from scratch at
            // OnPhysics2DStart, i.e. on the next Play.
            ImGui::Checkbox("Solid", &layer.Solid);
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat("Opacity", &layer.Opacity, 0.01f, 0.0f, 1.0f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat("Z Offset", &layer.ZOffset, 0.001f, -100.0f, 100.0f);

            ImGui::PopID();
        }

        if (tilemap.Layers.size() > 1 && ImGui::Button("Remove Active Layer"))
        {
            tilemap.Layers.erase(tilemap.Layers.begin() + static_cast<std::ptrdiff_t>(m_ActiveLayer));
            m_ActiveLayer = std::min(m_ActiveLayer, tilemap.Layers.size() - 1);
        }
    }

    void TilemapPainterPanel::DrawTilesetPicker()
    {
        auto& tilemap = m_TargetEntity.GetComponent<TilemapComponent>();

        const std::string label = tilemap.TilesetHandle != 0
                                      ? "Tileset: " + std::to_string(static_cast<u64>(tilemap.TilesetHandle))
                                      : std::string("Tileset: <none> (drop a .olotileset here)");
        ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f));
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            {
                std::filesystem::path assetPath = PathFromUtf8Payload(*payload);
                if (auto assetManager = Project::GetAssetManager().As<EditorAssetManager>())
                {
                    AssetHandle handle = assetManager->ImportAsset(assetPath);
                    if (handle != 0 && AssetManager::GetAssetType(handle) == AssetType::Tileset)
                    {
                        tilemap.TilesetHandle = handle;
                    }
                    else if (handle != 0)
                    {
                        OLO_WARN("Dropped asset is not a Tileset (type: {0})",
                                 AssetUtils::AssetTypeToString(AssetManager::GetAssetType(handle)));
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        auto tileset = AssetManager::GetAsset<Tileset>(tilemap.TilesetHandle);
        if (!tileset)
            return;

        auto texture = AssetManager::GetAsset<Texture2D>(tileset->GetTextureHandle());
        if (!texture)
        {
            ImGui::TextDisabled("Tileset texture is not loaded.");
            return;
        }
        // The atlas size lives on the texture; the picker is often the first thing
        // to touch a freshly created tileset, so fill it in here too rather than
        // drawing an empty grid.
        tileset->SetTextureSize(texture->GetWidth(), texture->GetHeight());

        const u32 columns = tileset->GetColumns();
        const u32 rows = tileset->GetRows();
        if (columns == 0 || rows == 0)
        {
            ImGui::TextDisabled("Atlas is smaller than one tile (%ux%u px, tile %ux%u px).",
                                tileset->GetTextureWidth(), tileset->GetTextureHeight(),
                                tileset->GetTileWidth(), tileset->GetTileHeight());
            return;
        }

        ImGui::DragFloat("Picker Zoom", &m_PickerTileSize, 1.0f, 8.0f, 128.0f, "%.0f px");
        // m_SelectedTile is the biased value (never 0 - the picker only ever
        // writes index + 1), so the display index is one less.
        ImGui::TextDisabled("Selected tile index %u (atlas has %u tiles)", m_SelectedTile > 0 ? m_SelectedTile - 1 : 0, columns * rows);

        // Go through ImGuiLayer::GetTextureID, not the raw renderer id: on the
        // Vulkan backend the id is a descriptor set, and handing ImGui a raw
        // handle (or a 0) binds a null descriptor. A 0 here means the backend
        // cannot show this texture, so fall back to a plain index list rather
        // than drawing an unusable grid.
        const u64 rawTextureID = ImGuiLayer::GetTextureID(*texture);
        if (rawTextureID == 0)
        {
            ImGui::TextDisabled("This backend cannot preview the atlas; select a tile by index.");
            int selected = static_cast<int>(m_SelectedTile) - 1;
            if (ImGui::DragInt("Tile Index", &selected, 1.0f, 0, static_cast<int>(columns * rows) - 1))
                m_SelectedTile = static_cast<u32>(std::clamp(selected, 0, static_cast<int>(columns * rows) - 1)) + 1;
            return;
        }
        const auto textureID = static_cast<ImTextureID>(rawTextureID);
        ImGui::BeginChild("TilesetGrid", ImVec2(0.0f, 240.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (u32 row = 0; row < rows; ++row)
        {
            for (u32 col = 0; col < columns; ++col)
            {
                const u32 index = row * columns + col;
                glm::vec2 uvMin{};
                glm::vec2 uvMax{};
                if (!tileset->GetTileUV(index, uvMin, uvMax))
                    continue;

                if (col > 0)
                    ImGui::SameLine();

                ImGui::PushID(static_cast<int>(index));
                // ImGui's UV origin is top-left while GetTileUV returns GL's
                // bottom-up rect, so the V pair is swapped back here.
                const bool selected = (m_SelectedTile == index + 1);
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.95f, 1.0f));
                if (ImGui::ImageButton("##tile", textureID,
                                       ImVec2(m_PickerTileSize, m_PickerTileSize),
                                       ImVec2(uvMin.x, uvMax.y), ImVec2(uvMax.x, uvMin.y)))
                {
                    m_SelectedTile = index + 1;
                }
                if (selected)
                    ImGui::PopStyleColor();

                if (ImGui::IsItemHovered())
                {
                    const TileInfo info = tileset->GetTileInfo(index);
                    std::string tooltip = "Tile " + std::to_string(index);
                    if (info.Solid)
                        tooltip += " - solid";
                    if (!info.Type.empty())
                        tooltip += " - " + info.Type;
                    ImGui::SetTooltip("%s", tooltip.c_str());
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void TilemapPainterPanel::OnImGuiRender()
    {
        if (!Visible)
            return;

        ImGui::Begin("Tilemap Painter", &Visible);

        if (!m_TargetEntity || !m_TargetEntity.HasComponent<TilemapComponent>())
        {
            ImGui::TextWrapped("Select an entity with a Tilemap component in the Scene Hierarchy to paint it.");
            // Painting is impossible without a target, so drop out of tool mode
            // rather than keeping the viewport's left-click hostage.
            m_Mode = Mode::Off;
            ImGui::End();
            return;
        }

        if (ImGui::BeginCombo("Tool", ModeLabel(m_Mode)))
        {
            for (auto mode : { Mode::Off, Mode::Paint, Mode::Erase, Mode::RectFill })
            {
                const bool selected = (m_Mode == mode);
                if (ImGui::Selectable(ModeLabel(mode), selected))
                    m_Mode = mode;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (m_Mode != Mode::Off)
            ImGui::TextDisabled("Left-click in the viewport paints; entity picking is suspended.");

        ImGui::Separator();
        DrawGridSettings();

        ImGui::Separator();
        DrawLayerList();

        ImGui::Separator();
        DrawTilesetPicker();

        ImGui::End();
    }
} // namespace OloEngine
