#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @brief Selects the high-level rendering strategy for the scene.
    //
    // The engine currently ships Forward and Forward+ paths.
    // The enum is intentionally extensible so that future paths
    // (Deferred, Visibility Buffer, Hybrid RT, etc.) can be added
    // without restructuring the existing pipeline.
    enum class RenderingPath : u8
    {
        Forward,     // Classic forward: all lights via UBO loop (low overhead, < ~8 lights)
        ForwardPlus, // Tiled Forward+: compute-culled per-tile light lists (scales to 256+ lights)

        // ----- Future paths (reserved, not yet implemented) -----
        // Deferred,         // G-buffer + fullscreen lighting pass
        // VisibilityBuffer, // Visibility buffer + material resolve
        // HybridRT,         // Forward+ with ray-traced shadows/GI
    };

    // @brief Global renderer settings that live alongside PostProcessSettings.
    //
    // Exposed in the editor via RendererSettingsPanel, persisted per-scene
    // or per-project. Controls pipeline-level knobs like the rendering path,
    // culling strategy, and debug overlays.
    struct RendererSettings
    {
        // --- Rendering path ---
        RenderingPath Path = RenderingPath::Forward;

        // --- Culling ---
        bool FrustumCullingEnabled = true;
        bool OcclusionCullingEnabled = false;

        // --- Depth pre-pass ---
        bool DepthPrepassEnabled = false;

        // --- Forward+ tuning (when Path == ForwardPlus or Auto) ---
        bool ForwardPlusAutoSwitch = true; // Auto-switch from Forward to Forward+ at threshold
        u32 ForwardPlusLightThreshold = 8;
        u32 ForwardPlusTileSize = 16; // 8, 16, or 32
        bool ForwardPlusDebugHeatmap = false;

        // --- Debug overlays ---
        bool WireframeOverlay = false;
    };
} // namespace OloEngine
