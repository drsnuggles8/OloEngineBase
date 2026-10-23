#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

// =============================================================================
// PickVolumes.h — the pure half of PickVolumesComponent (issue #1559).
//
// Scene::RaycastPickVolumes fits one capsule per bone of a skinned character
// (the groom body proxy's fit), carries them by this frame's palette and asks
// which one a ray enters first. The two pieces that need no scene -- the
// ray/capsule test and the region map -- live here so they are tested alone.
// =============================================================================

namespace OloEngine
{
    /// What a pick volume ray hit: the character, the bone whose capsule the ray
    /// entered first, the region that bone belongs to (its own name when it is
    /// in none), and where -- on the capsule's surface, padding included.
    struct PickVolumeHit
    {
        UUID Entity{ 0 };
        std::string Region;
        std::string Bone;
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
        f32 Distance = 0.0f;
    };

    namespace PickVolumes
    {
        /// The distance along `direction` (unit length) at which a ray from
        /// `origin` ENTERS the capsule a-b of `radius`; a ray that starts inside
        /// enters at 0. False when it misses, or the hit is past `maxDistance`.
        [[nodiscard]] bool RayCapsule(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a,
                                      const glm::vec3& b, f32 radius, f32 maxDistance, f32& outDistance);

        /// Möller–Trumbore: the distance along `direction` (unit length) at which a
        /// ray from `origin` crosses triangle a-b-c, from either side. False on a
        /// miss, a hit behind the origin or past `maxDistance`.
        [[nodiscard]] bool RayTriangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a,
                                       const glm::vec3& b, const glm::vec3& c, f32 maxDistance, f32& outDistance);

        /// "Ears: ear_L_01, ear_R_01; Chest: spine_03" -> bone name -> region.
        /// Whitespace around names is ignored; an empty name or a region with no
        /// bones contributes nothing; a bone listed twice keeps its first region.
        [[nodiscard]] std::unordered_map<std::string, std::string> ParseRegions(std::string_view text);
    } // namespace PickVolumes
} // namespace OloEngine
