#pragma once

// Pure, engine-light array-layer selection for olo_render_capture_target
// (issue #607) — the piece that makes a 2D-ARRAY render target (the CSM cascade
// array, the shadow atlas, and their comparison-OFF raw-depth views) capturable
// one layer at a time.
//
// Two silent-wrong-answer hazards live here, which is why this is a tested core
// rather than three lines inlined in the handler:
//
//   1. A render-graph LAYER VIEW ("ShadowMapCSMCascade3") resolves to its PARENT
//      texture object (RenderGraph::ResolveTexture, by design — that is what a
//      sampler binding wants). A CPU readback given only the parent id reads
//      layer 0, so capturing cascade 3 would hand back cascade 0's pixels with
//      no error at all. The view's own layer index must therefore be the DEFAULT
//      layer for that resource.
//   2. An out-of-range layer on a 4-cascade array must be an ERROR, not a
//      clamp: "you asked for cascade 7" is actionable, silently returning
//      cascade 0 (or 3) is the exact confidently-wrong answer this whole tool
//      family exists to eliminate.
//
// Only Core/Base.h and the standard library — no GL, no renderer.

#include "OloEngine/Core/Base.h"

#include <string>

namespace OloEngine::MCP::CaptureLayer
{
    // What the render graph says about one capturable resource.
    //   LayerCount — layers addressable THROUGH this resource name
    //                (RGResourceDesc::DepthOrLayers: 4 for the CSM array, 1 for
    //                the atlas, 1 for a per-cascade layer view, 1 for a plain 2D
    //                target).
    //   ViewLayer  — the layer this resource addresses within the parent texture
    //                object (RenderGraph::GetTextureViewLayerIndex): 3 for
    //                "ShadowMapCSMCascade3", 0 for everything that is not a
    //                layer/face view.
    struct TargetLayers
    {
        u32 LayerCount = 1;
        u32 ViewLayer = 0;
    };

    [[nodiscard]] inline bool IsArrayTarget(const TargetLayers& target)
    {
        return target.LayerCount > 1;
    }

    // The GL layer a capture should read, or an error explaining why the request
    // cannot be honoured. `Layer` is meaningful only when Error is empty.
    struct Selection
    {
        u32 Layer = 0;
        std::string Error;
        std::string Note; // set when the resolved layer is not the obvious 0
    };

    // `requested` is honoured only when `hasRequested`; otherwise the resource's
    // own view layer is used (0 for a non-view resource). The requested index is
    // relative to THIS resource, so it composes with a view: a layer view exposes
    // exactly one layer (index 0 = the view's own layer).
    [[nodiscard]] inline Selection SelectLayer(const TargetLayers& target, const std::string& name,
                                               bool hasRequested, long long requested)
    {
        Selection selection;

        if (!hasRequested)
        {
            selection.Layer = target.ViewLayer;
            if (target.ViewLayer != 0)
                selection.Note = "'" + name + "' is a view of layer " + std::to_string(target.ViewLayer) +
                                 " of its parent texture; that layer was captured.";
            else if (IsArrayTarget(target))
                selection.Note = "'" + name + "' is a " + std::to_string(target.LayerCount) +
                                 "-layer array; layer 0 was captured (pass 'layer' to pick another).";
            return selection;
        }

        if (requested < 0)
        {
            selection.Error = "Invalid 'layer' " + std::to_string(requested) + ": must be >= 0.";
            return selection;
        }
        if (!IsArrayTarget(target) && requested > 0)
        {
            // Deliberately an error: silently reading layer 0 of a non-array
            // target would look like a successful capture of the layer asked for.
            selection.Error = "'" + name + "' is not a texture array (it has a single layer), so 'layer' " +
                              std::to_string(requested) + " does not exist. Call olo_render_list_targets — an "
                                                          "array target reports its 'layers' count.";
            return selection;
        }
        if (static_cast<u32>(requested) >= target.LayerCount)
        {
            selection.Error = "'" + name + "' has " + std::to_string(target.LayerCount) + " layer(s), so 'layer' " +
                              std::to_string(requested) + " is out of range (valid: 0.." +
                              std::to_string(target.LayerCount - 1) + ").";
            return selection;
        }

        selection.Layer = target.ViewLayer + static_cast<u32>(requested);
        if (target.ViewLayer != 0)
            selection.Note = "'" + name + "' is a view of layer " + std::to_string(target.ViewLayer) +
                             " of its parent texture; the requested layer is relative to the view.";
        return selection;
    }
} // namespace OloEngine::MCP::CaptureLayer
