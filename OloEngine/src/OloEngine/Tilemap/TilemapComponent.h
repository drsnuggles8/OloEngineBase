#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/ComponentReflection.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace OloEngine
{
    // @brief One painted grid of tiles inside a TilemapComponent.
    //
    // `Tiles` is row-major, `Width * Height` entries, indexed `y * Width + x`
    // with (0, 0) at the bottom-left in world space. An entry is a *biased* tile
    // index: 0 means "no tile here", and any other value N addresses tileset tile
    // `N - 1`. The bias is what lets an empty cell be a plain zero, which keeps
    // the serialized form compact and a default-constructed layer empty.
    //
    // The vector is allowed to be shorter than `Width * Height` (a hand-edited or
    // truncated scene file); every read goes through TilemapComponent::GetTile,
    // which treats a missing entry as empty rather than repairing the layer on
    // load. Painting through SetTile grows it.
    //
    // Deliberately NOT named `*Component`: OloHeaderTool sweeps every struct with
    // that suffix into the generated component lists (CLAUDE.md, *Common pitfalls*).
    struct TileLayer
    {
        std::string Name = "Layer";
        std::vector<u32> Tiles;
        bool Visible = true;
        // Whether this layer contributes solid tiles to generated 2D colliders.
        bool Solid = false;
        f32 Opacity = 1.0f;
        // Draw-order nudge along Z, applied on top of the entity transform so
        // layers of a single tilemap can interleave with other sprites.
        f32 ZOffset = 0.0f;

        auto operator==(const TileLayer&) const -> bool = default;
    };

    // @brief A chunked, batched grid of tiles drawn through Renderer2D (issue #646).
    //
    // The entity transform places the map: tile (0, 0) starts at the transform's
    // origin and the grid grows towards +X / +Y, each tile `TileSize` world units
    // square. Layers share one grid and one tileset and are drawn in vector order,
    // so later layers paint over earlier ones.
    //
    // NOT to be confused with `TileRendererComponent` (Scene/Components.h), which
    // is a 3D modular-kit grid: it instances a `Ref<Mesh>` per cell with per-cell
    // materials. This one is the 2D sprite-atlas tilemap the issue asked for; the
    // two share a vocabulary and nothing else.
    //
    // Persistence is fully generated (OloHeaderTool): every member is public and
    // serializer-trivial, including `std::vector<TileLayer>`, which the #451
    // nested-struct slice already handles. There is no hand-written SceneSerializer
    // block and no `kComponentsCustomSerialize` entry.
    struct TilemapComponent
    {
        // The `.olotileset` asset supplying the atlas texture and per-tile
        // metadata. Resolved per frame via AssetManager rather than cached as a
        // Ref, so the component holds no runtime state and needs no hand-written
        // copy constructor / operator==.
        OLO_PROPERTY()
        AssetHandle TilesetHandle = 0;

        // Grid extent in tiles. Rejected rather than clamped on load: a corrupt
        // width would silently reinterpret every row of an otherwise-good layer,
        // so falling back to the constructor default is the honest outcome.
        //
        // Deliberately NOT OLO_PROPERTY: a generated setter is a bare
        // `comp.Width = value`, with no range check and — worse — no matching
        // resize of the layer storage, so it would silently re-index every row.
        // Changing the extent goes through Resize(), which moves the tiles with it.
        // Lua exposes these read-only for the same reason.
        OLO_SERIALIZE(Reject, Min = 1u, Max = 4096u)
        u32 Width = 32;
        OLO_SERIALIZE(Reject, Min = 1u, Max = 4096u)
        u32 Height = 32;

        // World-space edge length of one tile.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0001f, Max = 10000.0f)
        f32 TileSize = 1.0f;

        // Multiplied into every tile's vertex color, before per-layer Opacity.
        OLO_PROPERTY()
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };

        std::vector<TileLayer> Layers;

        // Build static Box2D collision from the solid tiles of every `Solid`
        // layer when the scene starts playing.
        OLO_PROPERTY()
        bool GenerateColliders = false;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 ColliderFriction = 0.5f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 ColliderRestitution = 0.0f;

        TilemapComponent() = default;
        TilemapComponent(const TilemapComponent&) = default;

        auto operator==(const TilemapComponent&) const -> bool = default;

        // Empty-cell sentinel and the bias between a stored entry and a tileset index.
        static constexpr u32 kEmptyTile = 0;

        [[nodiscard]] bool IsInBounds(u32 x, u32 y) const
        {
            return x < Width && y < Height;
        }

        [[nodiscard]] sizet CellIndex(u32 x, u32 y) const
        {
            return static_cast<sizet>(y) * static_cast<sizet>(Width) + static_cast<sizet>(x);
        }

        // @return the biased entry at (x, y), or kEmptyTile when the coordinate is
        //         outside the grid or the layer's storage is short of it.
        [[nodiscard]] u32 GetTile(sizet layer, u32 x, u32 y) const
        {
            if (layer >= Layers.size() || !IsInBounds(x, y))
                return kEmptyTile;
            const sizet index = CellIndex(x, y);
            const auto& tiles = Layers[layer].Tiles;
            return index < tiles.size() ? tiles[index] : kEmptyTile;
        }

        // @param value biased entry: 0 clears the cell, N places tileset tile N - 1.
        // @return false when the coordinate or layer is out of range (nothing written).
        bool SetTile(sizet layer, u32 x, u32 y, u32 value)
        {
            if (layer >= Layers.size() || !IsInBounds(x, y))
                return false;
            auto& tiles = Layers[layer].Tiles;
            const sizet needed = static_cast<sizet>(Width) * static_cast<sizet>(Height);
            if (tiles.size() < needed)
                tiles.resize(needed, kEmptyTile);
            tiles[CellIndex(x, y)] = value;
            return true;
        }

        // Adds a layer sized to the current grid and returns its index.
        sizet AddLayer(std::string name)
        {
            TileLayer layer;
            layer.Name = std::move(name);
            layer.Tiles.assign(static_cast<sizet>(Width) * static_cast<sizet>(Height), kEmptyTile);
            Layers.push_back(std::move(layer));
            return Layers.size() - 1;
        }

        // Largest grid the loader will accept, and the bound Resize enforces.
        // 4096x4096 tiles is already 16.7M entries per layer; beyond it a caller
        // (a script, a corrupt file) is asking for an allocation failure, not a map.
        static constexpr u32 kMaxExtent = 4096;

        // Re-sizes the grid, preserving the tiles that still fall inside it. A
        // no-op when the extent is unchanged or out of [1, kMaxExtent].
        void Resize(u32 newWidth, u32 newHeight);
    };
} // namespace OloEngine
