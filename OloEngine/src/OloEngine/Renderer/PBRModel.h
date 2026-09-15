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
    // reject-to-Legacy validation site (scene YAML, save-games, Lua). Appending
    // a model is a one-line bump here: since issue #996 the deferred G-Buffer
    // transport carries the model as a WHOLE integer in the RT2 flags lane
    // (oloEncodeGBufferPbrFlagsEx in PBRCommon.glsl, decoded by a plain `>> 6`
    // in DeferredLightingShared.glsl), so it can no longer truncate to Legacy on
    // the deferred path while Forward shades it correctly.
    inline constexpr i32 kPBRModelCount = 2;

    // The one real ceiling on that transport, and the site that rejects a model
    // above it LOUDLY rather than letting the G-Buffer silently remap it.
    //
    // The lane lives in RGBA16F alpha and carries `model * 64`, above the unlit
    // bit, a two-bit material kind and a three-bit skin-profile slot (issue
    // #1231 — the full field list is on oloEncodeGBufferPbrFlagsEx in
    // include/PBRCommon.glsl). IEEE half represents every integer exactly up to
    // 2048, so the largest model index that survives the write/read round-trip
    // un-truncated is 2048 / 64 - 1 = 31. Above that the encode rounds to a
    // neighbouring value and the decode hands the lighting pass a different
    // model — the exact silent Forward/Deferred divergence #996 removed.
    //
    // THIS NUMBER WAS 1023 BEFORE #1231, when the model was the only field above
    // the unlit bit. Five bits went to carrying what a surface IS and which
    // profile it names, which is a different question from which closure version
    // evaluates it and could not share the same field. Appending the 32nd
    // closure model is therefore a deliberate re-encoding of the lane (a wider
    // G-Buffer format, or a dedicated integer attachment), not a
    // `kPBRModelCount` bump, and this assertion is where that conversation
    // starts.
    inline constexpr i32 kPBRModelGBufferLaneMax = 31;
    static_assert(kPBRModelCount - 1 <= kPBRModelGBufferLaneMax,
                  "PBRModel index exceeds what the deferred G-Buffer flags lane (RGBA16F alpha, "
                  "model * 64) can carry exactly — see oloEncodeGBufferPbrFlagsEx in PBRCommon.glsl.");

} // namespace OloEngine
