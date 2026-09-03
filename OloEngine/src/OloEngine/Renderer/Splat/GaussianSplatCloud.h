#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::GaussianSplat
{
    // 3D Gaussian-splat cloud: the CPU-side asset representation and its PLY
    // importer (issue #971, a viability spike -- read
    // docs/adr/0018-gaussian-splats-are-an-offline-import-not-a-runtime-asset-type.md
    // before extending any of this).
    //
    // The source format is the one the INRIA "3D Gaussian Splatting for
    // Real-Time Radiance Field Rendering" trainer writes and every viewer reads:
    // a PLY with one `vertex` element whose per-splat float properties are
    //
    //     x y z                 world position
    //     nx ny nz              written, always zero, ignored by every consumer
    //     f_dc_0..2             band-0 spherical-harmonic colour
    //     f_rest_0..44          bands 1-3, view-dependent colour (optional)
    //     opacity               logit-encoded, sigmoid at load
    //     scale_0..2            log-encoded axis lengths, exp at load
    //     rot_0..3              orientation quaternion, stored w x y z
    //
    // Properties are matched BY NAME, not by offset: trainers differ on whether
    // f_rest_* is present at all and on the order of the trailing blocks, and a
    // by-offset reader silently produces a plausible-looking wrong cloud rather
    // than failing. `nx ny nz` in particular is dead weight that some exporters
    // omit.

    // What one splat costs on the GPU, and the only form the renderer sees.
    //
    // 32 bytes: position, an RGBA8 colour with opacity in alpha, and the upper
    // triangle of the symmetric world-space 3x3 covariance as six halves. The
    // covariance is baked at import (Sigma = R S S^T R^T) because scale and
    // rotation are never needed separately at draw time and re-deriving them
    // per frame is 9 multiplies and a quaternion normalise per splat.
    //
    // GPU-mirror layout rules apply (cpp-coding-quality.md 13): bare PascalCase
    // fields, explicit PadN.
    struct GpuSplat
    {
        glm::vec3 Position{ 0.0f };
        u32 ColorOpacity = 0; // RGBA8, alpha = opacity
        u32 CovXXXY = 0;      // half2(Sigma[0][0], Sigma[0][1])
        u32 CovXZYY = 0;      // half2(Sigma[0][2], Sigma[1][1])
        u32 CovYZZZ = 0;      // half2(Sigma[1][2], Sigma[2][2])
        u32 Pad0 = 0;
    };
    static_assert(sizeof(GpuSplat) == 32, "GpuSplat mirrors a std430 array element; see SplatSpike_Gaussian.glsl");
    static_assert(alignof(GpuSplat) == 4);

    // Bytes one splat occupies in a full INRIA PLY: 62 float properties.
    inline constexpr sizet kPlyBytesPerSplat = 62 * sizeof(f32);

    // Band-0 spherical-harmonic basis value. Colour = 0.5 + kShC0 * f_dc.
    inline constexpr f32 kShC0 = 0.28209479177387814f;

    // Result of an import. `Ok == false` leaves the cloud empty and `Error`
    // holding a sentence that names the file and what was wrong with it --
    // asset-degradation-and-constructor-preconditions.md: an importer reports,
    // it never half-loads.
    struct LoadResult
    {
        bool Ok = false;
        std::string Error;
        u32 SplatsRead = 0;
        // Bytes the source file spent on properties this representation drops
        // (normals and the f_rest_* SH bands). Reported so the writeup's memory
        // comparison is measured rather than asserted.
        sizet DiscardedSourceBytes = 0;
    };

    class SplatCloud
    {
      public:
        SplatCloud() = default;

        // Reads `path` as an INRIA-layout PLY. Accepts `binary_little_endian`
        // and `ascii`; rejects `binary_big_endian` explicitly rather than
        // reading it wrong.
        [[nodiscard]] auto LoadPly(const std::filesystem::path& path) -> LoadResult;

        // Same parse over an in-memory file image, so tests do not need a
        // temp file and a fuzzer can drive it directly.
        [[nodiscard]] auto LoadPlyFromMemory(std::span<const u8> bytes, std::string_view sourceName) -> LoadResult;

        [[nodiscard]] auto Splats() const -> std::span<const GpuSplat>
        {
            return m_Splats;
        }
        [[nodiscard]] auto Count() const -> u32
        {
            return static_cast<u32>(m_Splats.size());
        }
        [[nodiscard]] auto Empty() const -> bool
        {
            return m_Splats.empty();
        }
        [[nodiscard]] auto Bounds() const -> const BoundingBox&
        {
            return m_Bounds;
        }

        // Largest 3-sigma world-space radius over the cloud. The renderer needs
        // it to size a conservative near-plane guard band; the LOD pass needs it
        // to bound projected extent without touching every splat.
        [[nodiscard]] auto MaxSplatRadius() const -> f32
        {
            return m_MaxRadius;
        }

        [[nodiscard]] auto GpuBytes() const -> sizet
        {
            return m_Splats.size() * sizeof(GpuSplat);
        }

        // Builds a cloud directly from unpacked parameters. This is the seam the
        // PLY reader itself goes through, so a procedurally generated cloud and
        // an imported one are byte-identical downstream -- which is what lets
        // the perf numbers use a 500k generated cloud while the format evidence
        // uses a small checked-in fixture.
        void Build(std::span<const glm::vec3> positions,
                   std::span<const glm::vec3> shDc,
                   std::span<const f32> logitOpacity,
                   std::span<const glm::vec3> logScale,
                   std::span<const glm::vec4> rotationWXYZ);

        void Clear();

      private:
        std::vector<GpuSplat> m_Splats;
        BoundingBox m_Bounds;
        f32 m_MaxRadius = 0.0f;
    };

    // -------------------------------------------------------------------------
    // Free functions the importer uses, exposed because they are exactly what a
    // contract test has to pin -- each is a place where a wrong constant makes a
    // cloud that renders, and renders wrong.
    // -------------------------------------------------------------------------

    // opacity property -> alpha. The trainer stores the logit.
    [[nodiscard]] auto SigmoidOpacity(f32 logit) -> f32;

    // f_dc_* -> linear RGB in 0..1.
    [[nodiscard]] auto ShDcToColor(const glm::vec3& shDc) -> glm::vec3;

    // (log scale, quaternion w x y z) -> world covariance Sigma = R S S^T R^T.
    // Returns the upper triangle in the order the GPU record packs it:
    // xx, xy, xz, yy, yz, zz.
    [[nodiscard]] auto CovarianceFromScaleRotation(const glm::vec3& logScale, const glm::vec4& rotationWXYZ)
        -> std::array<f32, 6>;

    // Packs six covariance terms into the record's three u32 half2 slots.
    void PackCovariance(const std::array<f32, 6>& sigma, u32& covXXXY, u32& covXZYY, u32& covYZZZ);

    // Inverse of PackCovariance, for tests and for the CPU LOD pass.
    [[nodiscard]] auto UnpackCovariance(u32 covXXXY, u32 covXZYY, u32 covYZZZ) -> std::array<f32, 6>;

    [[nodiscard]] auto PackColorOpacity(const glm::vec3& linearRgb, f32 opacity) -> u32;
    [[nodiscard]] auto UnpackColorOpacity(u32 packed) -> glm::vec4;
} // namespace OloEngine::GaussianSplat
