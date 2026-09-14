#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>
#include <array>
#include <cstddef>

namespace OloEngine::ReSTIR::PT
{
    inline constexpr u32 kLayoutVersion = 1u;
    inline constexpr u32 kMaterialTexturesFlag = 1u;
    inline constexpr u32 kEnvironmentCubeFlag = 2u;
    inline constexpr u32 kMaximumSecondaryVertices = 3u;
    inline constexpr u32 kCounterCount = 16u;

    // std430, by-address storage. Coordinates are relative to the TLAS origin.
    // The sampler's raw uniforms survive unchanged during random replay.
    struct alignas(16) VertexRecord
    {
        glm::vec4 PositionRoughness{};
        glm::vec4 GeometricNormalMetallic{};
        glm::vec4 ShadingNormalClosure{};
        glm::vec4 Albedo{};
        glm::vec4 Incoming{};
        glm::vec4 Randoms{};
        glm::uvec4 Identity{}; // instance, geometry, primitive, material
    };
    struct alignas(16) EndpointRecord
    {
        glm::vec4 PositionKind{}; // 0 invalid, 1 environment, 2 emitter, 3 punctual, 4 directional
        glm::vec4 NormalTwoSided{};
        glm::vec4 RadianceDensity{};
        glm::vec4 Randoms{}; // selection, point u/v, discrete selection mass
        glm::uvec4 Identity{};
    };
    struct alignas(16) PathRecord
    {
        std::array<VertexRecord, 4> Vertices{}; // primary plus three secondary vertices
        EndpointRecord Endpoint{};
        glm::uvec4 Metadata{}; // secondary count, endpoint strategy (0 NEE/1 BSDF), epoch low/high
        glm::uvec4 Lineage{};  // frame, pixel, mapping, high 16 bits layout version; low flags: 1 receiver, 2 selected, 4 temporal ancestry
        glm::vec4 State{};     // W, deterministic M, extended target, log J
        glm::vec4 Value{};     // extended integrand RGB, raw candidate sample variance
    };
    struct alignas(16) Parameters
    {
        glm::mat4 InvView{ 1.0f };
        glm::mat4 InvProjection{ 1.0f };
        glm::mat4 View{ 1.0f };
        glm::uvec4 TlasAddressAndFrame{};
        glm::uvec4 SlotCounts{};
        glm::uvec4 EmissiveTable{};
        glm::uvec4 MaterialTable{};
        glm::uvec4 SourceAddresses{};      // canonical pool xy, neighbour pool zw
        glm::uvec4 DestinationAddresses{}; // output pool xy, counters zw
        glm::uvec4 HistoryAddresses{};     // previous initial pool xy, current initial pool zw
        glm::uvec4 Counts{};               // candidates, stage 0..3, mapping mask (1/2/4), epoch low
        glm::vec4 Params{};                // normal bias, ray epsilon, max distance, connection roughness
        glm::vec4 EstimatorParams{};       // emissive area PDF, environment cube intensity, clamp, radius px
        glm::vec4 Environment{};
        glm::vec4 Screen{};
        glm::uvec4 Reuse{}; // temporal, spatial, valid history, epoch high
        glm::vec4 Debug{};  // view, fixed seed, M cap, layout version
    };
    static_assert(sizeof(VertexRecord) == 112);
    static_assert(sizeof(EndpointRecord) == 80);
    static_assert(sizeof(PathRecord) == 592);
    static_assert(offsetof(PathRecord, Endpoint) == 448);
    static_assert(offsetof(PathRecord, State) == 560);
    static_assert(sizeof(Parameters) == 416);
    static_assert(offsetof(Parameters, SourceAddresses) == 256);
} // namespace OloEngine::ReSTIR::PT
