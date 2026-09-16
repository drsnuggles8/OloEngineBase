#pragma once

// =============================================================================
// GroomStrandRequest.h — one groom the frame wants drawn. Issue #1246.
//
// The producer/transport/consumer split is the one RayTracedShadowLightRequest
// established (Renderer/Shadow/ShadowTechnique.h): Scene publishes these
// because it is the only place that holds both the component and the resolved
// asset; Renderer3D carries them; GroomRenderPass decides what each one
// actually gets, because it is the only place that knows what the frame
// resolved. The struct lives beside the groom rather than beside the pass so
// none of the three has to include the render-graph headers to speak about it.
//
// THE ASSET IS RESOLVED BY THE PRODUCER, not by the pass. Two reasons, and the
// second is the one that bites: an AssetManager lookup on the render thread is
// a lock the frame does not need, and a groom built at runtime lives in the
// manager's memory-asset map and in no registry at all — which is exactly the
// case a pass re-resolving by handle got wrong for the debug preview (see
// Scene.cpp's note on IsAssetHandleValid).
// =============================================================================

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
// The COMPLETE GroomAsset, not a forward declaration: Ref<T> needs T to be a
// complete RefCounted to construct, copy or destroy, so a forward declaration
// here fails in every translation unit that merely HOLDS one of these — which
// after Renderer3D.h includes it is most of the renderer.
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/glm.hpp>

namespace OloEngine
{

    struct GroomStrandRequest
    {
        /// The cooked groom. Never null in a published request — Scene drops
        /// the entity rather than forwarding a null, so the pass has no
        /// "asset missing" branch to get wrong.
        Ref<GroomAsset> Groom;

        /// The asset's handle, carried ONLY as the strand-buffer cache key.
        /// A memory-only groom has a handle too, so this is a usable key even
        /// where the registry knows nothing about the asset.
        AssetHandle Handle = 0;

        /// Composed world transform, and the same from last frame. The
        /// previous one drives the velocity buffer; it aliases the current one
        /// on an entity's first frame, so velocity starts at zero rather than
        /// undefined.
        glm::mat4 Transform{ 1.0f };
        glm::mat4 PreviousTransform{ 1.0f };

        /// Neutral albedo. #1246 renders an unlit coat on purpose — see
        /// GroomStrand.glsl — so this is the only colour in play.
        glm::vec3 Color{ 0.55f, 0.48f, 0.42f };

        /// Root-to-tip brightness floor, the same geometric ramp the debug
        /// preview uses. 1.0 is a flat colour.
        f32 RampFloor = 0.35f;

        /// Multiplies the cooked object-space diameters. An authoring lever
        /// for grooms exported at a different unit scale, not a quality knob.
        f32 WidthScale = 1.0f;

        /// Alpha cutoff for the OpaqueRibbon tier.
        f32 AlphaCutoff = 0.5f;

        /// What this groom ASKED for. What it gets is
        /// SelectGroomComposition's answer, which the pass records.
        GroomCompositionMode RequestedMode = GroomCompositionMode::StochasticAlpha;

        /// Editor picking id.
        i32 EntityID = -1;

        /// How much of the groom to turn into geometry.
        GroomStrandBuildSettings Build;
    };
} // namespace OloEngine
