#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{

    // PBR closure model version (issue #975).
    //
    // The closure a material shades with is an explicit, serialized choice, so a
    // corrected BRDF can ship without silently changing every existing scene and
    // golden. Legacy is the default: it is the exact Cook-Torrance closure the
    // engine has always shipped (Schlick-GGX k geometry term, EPSILON-clamped
    // GGX denominator, no energy compensation), frozen bit-for-bit. ClosureV2 is
    // the per-material opt-in: one geometry term (height-correlated Smith
    // visibility), alpha-clamped near-mirror handling, and Kulla-Conty
    // multiple-scattering energy compensation.
    //
    // Mirrored in GLSL as OLO_PBR_MODEL_* (include/PBRCommon.glsl) and carried
    // to the GPU in PBRMaterialUBO::PBRModel; the CPU reference path tracer
    // dispatches on ReferenceMaterial::Model through PBRClosureBSDF.h. The
    // numbering is on disk (scene YAML, save-games) — append, never renumber.
    enum class PBRModel : u8
    {
        Legacy = 0,
        ClosureV2 = 1
    };

    // One past the last valid model — the shared upper bound for every
    // reject-to-Legacy validation site (scene YAML, save-games, Lua). When
    // appending a model, bump this, and ALSO widen the deferred G-Buffer
    // transport: the RT2 flags lane carries the model in a SINGLE bit
    // (oloEncodeGBufferFlags in PBRCommon.glsl / the decode in
    // DeferredLightingShared.glsl), so a third model needs that lane widened
    // in the same change or it silently shades Legacy on the deferred path
    // while Forward shades it correctly.
    inline constexpr i32 kPBRModelCount = 2;

} // namespace OloEngine
