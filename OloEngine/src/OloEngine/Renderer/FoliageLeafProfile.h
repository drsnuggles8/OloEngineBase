#pragma once

// =============================================================================
// FoliageLeafProfile.h — the per-layer half of the vegetation material, and the
// small integer slot the deferred path names it by. Issue #1234.
//
// WHY A SLOT TABLE AT ALL. The transmission lobe's shape (tint, strength,
// distortion, exponent, wrap, environment scale) is authored PER LAYER, and the
// deferred lighting pass is a fullscreen draw that has no per-layer state — the
// same problem #1231 hit with skin profiles, solved the same way, deliberately:
// the issue's scope boundary says to reuse the material-kind plumbing from
// skin. The G-Buffer carries the slot in the SAME three-bit field skin's
// profile slot uses (bits 3..5 of the RT2 flags lane), read only under the
// MaterialKind::Foliage test — exactly as skin's is read only under
// MaterialKind::Skin. One field, two tenants, never both on one pixel, because
// a pixel has one kind.
//
// WHAT IS *NOT* HERE. The per-PIXEL half of the surface — the sampled normal,
// roughness and thickness — never goes through a slot: it is per pixel, and the
// G-Buffer already carries it (normal and roughness in RT1, thickness in RT5's
// red channel). Only the parameters that are constant across a layer are
// interned.
//
// INTERNED BY VALUE, not by handle. A FoliageLayer's leaf material is authored
// inline (FoliageLayer.h), not as an asset, so there is no handle to key on —
// two layers that authored identical parameters SHOULD share a slot, and this
// is what makes a forest of one species cost one.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Same width and the same "none" code as the skin profile slot, because it
    // is the SAME three-bit G-Buffer field. Keeping the constants separate
    // rather than aliasing skin's says that the two are tenants of one field
    // rather than one concept, which is what stops a future widening of one
    // from silently widening the other.
    inline constexpr u32 kFoliageLeafSlotBits = 3;
    inline constexpr u32 kFoliageLeafSlotNone = (1u << kFoliageLeafSlotBits) - 1u; // 7
    inline constexpr u32 kMaxFoliageLeafSlots = kFoliageLeafSlotNone;              // slots 0..6

    // The layer-constant parameters of the transmission lobe. See
    // oloFoliageTransmission in assets/shaders/include/FoliageSurface.glsl for
    // what each does to the image; this struct is the CPU mirror of the two
    // vec4s the deferred controls block publishes per slot.
    struct FoliageLeafProfile
    {
        glm::vec3 TransmissionColor{ 0.42f, 0.62f, 0.18f };
        // 0 means "not a leaf material". A profile with zero strength is never
        // interned — the layer writes MaterialKind::Generic instead, so a slot
        // is not spent on a material that transmits nothing.
        f32 TransmissionStrength = 0.0f;
        f32 Distortion = 0.35f;
        f32 Power = 4.0f;
        f32 Wrap = 0.5f;
        f32 Ambient = 0.35f;

        [[nodiscard]] bool IsLeaf() const noexcept
        {
            return TransmissionStrength > 0.0f;
        }

        // Bitwise float comparison per cpp-coding-quality §2a. Interning is an
        // identity question ("are these the same authored material?"), not a
        // similarity one, so an epsilon would be wrong here in both directions:
        // it would merge two layers an author deliberately separated by a hair,
        // and it would still miss the case it was added for.
        auto operator==(const FoliageLeafProfile& o) const -> bool
        {
            return Math::BitwiseEqual(TransmissionColor, o.TransmissionColor) &&
                   Math::BitwiseEqual(TransmissionStrength, o.TransmissionStrength) &&
                   Math::BitwiseEqual(Distortion, o.Distortion) && Math::BitwiseEqual(Power, o.Power) &&
                   Math::BitwiseEqual(Wrap, o.Wrap) && Math::BitwiseEqual(Ambient, o.Ambient);
        }
    };

    // The two vec4 lanes a slot occupies in the deferred controls block. Kept
    // as a function rather than duplicated at the publish site so the CPU
    // packing and the GLSL unpacking have one definition between them.
    //
    //   lane 0: rgb = tint * strength (pre-multiplied — the shader never sees
    //           the two apart, and pre-multiplying here means the forward path
    //           and the deferred path cannot differ on the order)
    //   lane 1: x = distortion, y = power, z = wrap, w = environment scale,
    //           which is exactly the `lobe` vec4 oloFoliageTransmission takes.
    [[nodiscard]] inline glm::vec4 FoliageLeafProfileTintLane(const FoliageLeafProfile& p) noexcept
    {
        return glm::vec4(p.TransmissionColor * p.TransmissionStrength, p.TransmissionStrength);
    }

    [[nodiscard]] inline glm::vec4 FoliageLeafProfileLobeLane(const FoliageLeafProfile& p) noexcept
    {
        return glm::vec4(p.Distortion, p.Power, p.Wrap, p.Ambient);
    }

} // namespace OloEngine
