#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    // @brief Selects the high-level rendering strategy for the scene.
    //
    // The engine ships Forward, Forward+ and Deferred paths. Deferred reuses
    // the Forward+ tile-culling infrastructure to drive the tiled-deferred
    // lighting pass — selecting Deferred implicitly activates the tile
    // culling compute so the G-Buffer lighting shader can sample per-tile
    // light lists.
    //
    // The enum is intentionally extensible so that future paths
    // (Visibility Buffer, Hybrid RT, etc.) can be added without
    // restructuring the existing pipeline.
    enum class RenderingPath : u8
    {
        Forward,     // Classic forward: all lights via UBO loop (low overhead, < ~8 lights)
        ForwardPlus, // Tiled Forward+: compute-culled per-tile light lists (scales to 256+ lights)
        Deferred,    // G-buffer + tiled deferred lighting (see docs/deferred-renderer.md)

        // ----- Future paths (reserved, not yet implemented) -----
        // VisibilityBuffer, // Visibility buffer + material resolve
        // HybridRT,         // Forward+ with ray-traced shadows/GI
    };

    // @brief Deferred renderer configuration.
    //
    // Exposed in RendererSettingsPanel when Path == Deferred. Full pipeline
    // support (G-Buffer write, lighting, decals, MSAA, OIT) arrives in
    // subsequent phases; the struct ships now so settings can be serialized
    // and so UI controls have a stable target.
    struct DeferredSettings
    {
        // Multisample sample count for the G-Buffer. 1 = no MSAA.
        // Supported values: 1, 2, 4, 8 — validated at G-Buffer creation time.
        u32 MSAASampleCount = 1;

        // Per-sample deferred lighting. When true (and MSAASampleCount > 1),
        // DeferredLightingPass selects the MSAA shader variant and evaluates
        // PBR lighting for every sub-sample, averaging the result. This
        // produces correct anti-aliasing at material boundaries at the cost
        // of roughly Nx shading rate on covered pixels. When false, the
        // G-Buffer is resolved pre-lighting (hardware edge averaging only).
        bool PerSampleLighting = true;

        // Debug view of a single G-Buffer channel (0 = off, 1 = albedo,
        // 2 = normal, 3 = roughness/metallic/AO, 4 = emissive, 5 = velocity).
        u32 DebugChannel = 0;

        // Route decal pass into G-Buffer writes instead of post-lighting.
        bool GBufferDecalsEnabled = true;

        // Enable light-probe contribution to the deferred ambient term.
        // When true, `DeferredLighting.glsl` samples the active light-probe
        // volume's SH coefficients (bound by `Scene::OnUpdateRender`) and
        // blends them trilinearly with the global IBL cubemap. When false
        // (or when no volume is active), the shader falls back to the
        // global IBL cubemap only. Runtime-switchable.
        bool EnableLightProbes = true;
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
        // Lower-bound hysteresis: once Forward+ is active, stay active until
        // the total light count drops to/below this value. Prevents per-frame
        // path oscillation when light counts hover around the upgrade
        // threshold. Ignored if >= ForwardPlusLightThreshold (no hysteresis).
        u32 ForwardPlusLightThresholdDown = 4;
        u32 ForwardPlusTileSize = 16; // 8, 16, or 32
        bool ForwardPlusDebugHeatmap = false;

        // --- Deferred tuning (when Path == Deferred) ---
        DeferredSettings Deferred;

        // --- Transparency ---
        // Weighted-blended OIT for transparents (particles, decals). The OIT
        // shaders are path-agnostic — this is exposed at top level so it can
        // be toggled in Forward, Forward+, and Deferred paths.
        bool OITEnabled = false;

        // --- Debug overlays ---
        bool WireframeOverlay = false;
        bool ShowGrid = true;
        bool ShowPhysicsColliders = false;
        bool ShowLightGizmos = true;
        // World-origin XYZ axis helper (small RGB axes at (0,0,0) plus negative-axis dashes).
        // Frequently mistaken for a light gizmo because the green Y axis reads as yellow in HDR.
        bool ShowWorldAxisHelper = true;
        // Yellow/cyan frustum wireframe drawn at every CameraComponent's transform. Useful when
        // composing scene cameras, noisy when you're just walking around the editor camera.
        bool ShowCameraFrustums = true;
        // Visualise the per-object screen-space velocity buffer in Forward
        // and Forward+ paths. Ignored in Deferred (which has its own
        // G-Buffer velocity debug channel — DeferredSettings::DebugChannel
        // = 5). When enabled, SceneRenderPass blits the scene FB's RG16F
        // velocity attachment into colour[0] right after scene rendering
        // (pre post-process), so the viewport shows the velocity buffer.
        bool DebugVelocityOverlayForward = false;
    };
} // namespace OloEngine
