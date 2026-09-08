#include "OloEnginePCH.h"

#include "OloEngine/Renderer/PathTracing/ReferenceScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace OloEngine::PathTracing
{
    namespace
    {
        // A world AABB is only usable as a traversal bound if it is finite;
        // an instance whose transform produced a NaN corner would otherwise
        // poison every slab test that touches it.
        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        [[nodiscard]] bool IsFinite(const BoundingBox& box)
        {
            return IsFinite(box.Min) && IsFinite(box.Max);
        }
    } // namespace

    // =========================================================================
    // ReferenceGeometry
    // =========================================================================

    ReferenceGeometry::ReferenceGeometry(std::vector<Vertex> vertices, std::vector<u32> indices)
        : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
    {
        m_BVH.Build(m_Vertices.data(), m_Vertices.size(), m_Indices.data(), m_Indices.size());
        m_LocalBounds = m_BVH.IsBuilt() ? m_BVH.GetBounds() : BoundingBox(glm::vec3(0.0f), glm::vec3(0.0f));
    }

    ReferenceTexture ReferenceTexture::FromRgba8(u32 width, u32 height, std::span<const u8> rgba, bool srgb)
    {
        ReferenceTexture texture;
        if (width == 0 || height == 0 || rgba.size() < static_cast<sizet>(width) * height * 4u)
            return texture;
        texture.Width = width;
        texture.Height = height;
        texture.Texels.resize(static_cast<sizet>(width) * height);
        // The sRGB EOTF, per channel, alpha untouched: what an sRGB image
        // format decodes to on the way into the filter.
        const auto decode = [srgb](u8 value) -> f32
        {
            const f32 c = static_cast<f32>(value) / 255.0f;
            if (!srgb)
                return c;
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        for (sizet i = 0; i < texture.Texels.size(); ++i)
        {
            texture.Texels[i] = glm::vec4(decode(rgba[i * 4 + 0]), decode(rgba[i * 4 + 1]), decode(rgba[i * 4 + 2]),
                                          static_cast<f32>(rgba[i * 4 + 3]) / 255.0f);
        }
        return texture;
    }

    glm::vec4 ReferenceTexture::SampleBilinear(const glm::vec2& uv) const
    {
        if (Width == 0 || Height == 0 || Texels.empty())
            return glm::vec4(1.0f);
        // REPEAT addressing is applied to the coordinate FIRST, in float:
        // a UV far outside [0, 1) or a non-finite one would otherwise reach
        // the float-to-integer conversion below out of i32's range, which is
        // undefined behaviour. A non-finite coordinate reads texel 0, the
        // way the GPU's REPEAT wrap of a NaN is some texel rather than a fault.
        const auto repeat = [](f32 coordinate) -> f32
        {
            if (!std::isfinite(coordinate))
                return 0.0f;
            const f32 wrapped = coordinate - std::floor(coordinate);
            return wrapped < 1.0f ? wrapped : 0.0f; // 1 - ulp rounds up to 1.0 for huge inputs
        };
        // Texel centres at (i + 0.5) / size.
        const f32 x = repeat(uv.x) * static_cast<f32>(Width) - 0.5f;
        const f32 y = repeat(uv.y) * static_cast<f32>(Height) - 0.5f;
        const f32 x0f = std::floor(x);
        const f32 y0f = std::floor(y);
        const f32 fx = x - x0f;
        const f32 fy = y - y0f;
        // x0f is in [-1, Width - 1] after the wrap, so the conversion is
        // in range; the -1 (a coordinate left of the first centre) wraps to
        // the last texel.
        const auto wrap = [](i32 index, u32 size) -> u32
        {
            const i32 s = static_cast<i32>(size);
            return static_cast<u32>(((index % s) + s) % s);
        };
        const auto x0 = static_cast<i32>(x0f);
        const auto y0 = static_cast<i32>(y0f);
        const u32 ix0 = wrap(x0, Width);
        const u32 ix1 = wrap(x0 + 1, Width);
        const u32 iy0 = wrap(y0, Height);
        const u32 iy1 = wrap(y0 + 1, Height);
        const glm::vec4& t00 = Texels[static_cast<sizet>(iy0) * Width + ix0];
        const glm::vec4& t10 = Texels[static_cast<sizet>(iy0) * Width + ix1];
        const glm::vec4& t01 = Texels[static_cast<sizet>(iy1) * Width + ix0];
        const glm::vec4& t11 = Texels[static_cast<sizet>(iy1) * Width + ix1];
        return glm::mix(glm::mix(t00, t10, fx), glm::mix(t01, t11, fx), fy);
    }

    // =========================================================================
    // ReferenceEnvironmentCubemap
    // =========================================================================

    ReferenceEnvironmentCubemap ReferenceEnvironmentCubemap::FromFacesRgba32F(u32 faceSize, std::span<const f32> rgbaFaces)
    {
        ReferenceEnvironmentCubemap cube;
        const sizet needed = static_cast<sizet>(kFaceCount) * faceSize * faceSize * 4u;
        if (faceSize == 0 || rgbaFaces.size() < needed)
            return cube;
        cube.FaceSize = faceSize;
        cube.Texels.resize(static_cast<sizet>(kFaceCount) * faceSize * faceSize);
        for (sizet i = 0; i < cube.Texels.size(); ++i)
        {
            // A non-finite or negative texel is dropped to zero rather than
            // carried into the integrator: one NaN in a sky would poison every
            // baked texel in the scene, and an .hdr with a stray negative is a
            // real thing. Clamping here rather than at every escape keeps the
            // hot path branch-free.
            const f32 r = rgbaFaces[i * 4 + 0];
            const f32 g = rgbaFaces[i * 4 + 1];
            const f32 b = rgbaFaces[i * 4 + 2];
            cube.Texels[i] = glm::vec3(std::isfinite(r) && r > 0.0f ? r : 0.0f, std::isfinite(g) && g > 0.0f ? g : 0.0f,
                                       std::isfinite(b) && b > 0.0f ? b : 0.0f);
        }
        return cube;
    }

    ReferenceEnvironmentCubemap ReferenceEnvironmentCubemap::Constant(const glm::vec3& radiance)
    {
        ReferenceEnvironmentCubemap cube;
        cube.FaceSize = 1;
        // The SAME guard FromFacesRgba32F applies, and for the same reason: a
        // NaN here would otherwise pass IsValid(), sail past the builder's
        // malformed-cubemap fallback, and NaN every texel of the atlas. A
        // constructor that validates one way in and not the other is a hole
        // whose only symptom is a poisoned bake.
        const auto clean = [](f32 c)
        { return std::isfinite(c) && c > 0.0f ? c : 0.0f; };
        cube.Texels.assign(kFaceCount, glm::vec3(clean(radiance.x), clean(radiance.y), clean(radiance.z)));
        return cube;
    }

    glm::vec3 ReferenceEnvironmentCubemap::Sample(const glm::vec3& direction) const
    {
        if (!IsValid())
            return glm::vec3(0.0f);
        if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z))
            return glm::vec3(0.0f);

        const glm::vec3 a(std::abs(direction.x), std::abs(direction.y), std::abs(direction.z));
        const f32 ma = std::max({ a.x, a.y, a.z });
        if (!(ma > 0.0f))
            return glm::vec3(0.0f);

        // The GL cubemap face table, verbatim (OpenGL 4.6 spec, table 8.19).
        // `sc`/`tc` are the coordinates within the face and `ma` the major
        // axis' magnitude; s and t are (coord/ma + 1) / 2.
        u32 face = 0;
        f32 sc = 0.0f;
        f32 tc = 0.0f;
        if (a.x >= a.y && a.x >= a.z)
        {
            if (direction.x > 0.0f)
            {
                face = 0; // +X
                sc = -direction.z;
                tc = -direction.y;
            }
            else
            {
                face = 1; // -X
                sc = direction.z;
                tc = -direction.y;
            }
        }
        else if (a.y >= a.z)
        {
            if (direction.y > 0.0f)
            {
                face = 2; // +Y
                sc = direction.x;
                tc = direction.z;
            }
            else
            {
                face = 3; // -Y
                sc = direction.x;
                tc = -direction.z;
            }
        }
        else
        {
            if (direction.z > 0.0f)
            {
                face = 4; // +Z
                sc = direction.x;
                tc = -direction.y;
            }
            else
            {
                face = 5; // -Z
                sc = -direction.x;
                tc = -direction.y;
            }
        }

        const f32 s = 0.5f * (sc / ma + 1.0f);
        const f32 t = 0.5f * (tc / ma + 1.0f);

        // Bilinear with CLAMP at the face borders — no seam filtering, on
        // purpose (see the header). Texel centres at (i + 0.5) / size.
        const f32 x = s * static_cast<f32>(FaceSize) - 0.5f;
        const f32 y = t * static_cast<f32>(FaceSize) - 0.5f;
        const f32 x0f = std::floor(x);
        const f32 y0f = std::floor(y);
        const f32 fx = x - x0f;
        const f32 fy = y - y0f;
        const auto clampIndex = [this](f32 index) -> u32
        {
            const f32 clamped = std::clamp(index, 0.0f, static_cast<f32>(FaceSize - 1u));
            return static_cast<u32>(clamped);
        };
        const u32 ix0 = clampIndex(x0f);
        const u32 ix1 = clampIndex(x0f + 1.0f);
        const u32 iy0 = clampIndex(y0f);
        const u32 iy1 = clampIndex(y0f + 1.0f);

        const sizet faceBase = static_cast<sizet>(face) * FaceSize * FaceSize;
        // All four taps are the same texel — a 1x1 face, or a coordinate that
        // clamped into a corner. Returning it directly is not an optimisation:
        // `mix(v, v, a)` is `v * (1 - a) + v * a`, which is v mathematically
        // and up to an ulp off in f32. The reduction "a constant cubemap
        // behaves exactly like the uniform environment" is asserted bit-exactly
        // (ReferenceEnvironment.ConstantCubemapEvaluatesLikeTheUniformEnvironment)
        // and this is what makes that true rather than nearly true.
        if (ix0 == ix1 && iy0 == iy1)
            return Texels[faceBase + static_cast<sizet>(iy0) * FaceSize + ix0];

        const glm::vec3& t00 = Texels[faceBase + static_cast<sizet>(iy0) * FaceSize + ix0];
        const glm::vec3& t10 = Texels[faceBase + static_cast<sizet>(iy0) * FaceSize + ix1];
        const glm::vec3& t01 = Texels[faceBase + static_cast<sizet>(iy1) * FaceSize + ix0];
        const glm::vec3& t11 = Texels[faceBase + static_cast<sizet>(iy1) * FaceSize + ix1];
        return glm::mix(glm::mix(t00, t10, fx), glm::mix(t01, t11, fx), fy);
    }

    glm::vec3 ReferenceEnvironment::Evaluate(const glm::vec3& direction) const
    {
        return Intensity * (Cubemap ? Cubemap->Sample(direction) : Radiance);
    }

    bool ReferenceGeometry::GetTriangleVertices(u32 triangleIndex, u32& i0, u32& i1, u32& i2) const
    {
        const sizet base = static_cast<sizet>(triangleIndex) * 3;
        if (base + 2 >= m_Indices.size())
            return false;
        i0 = m_Indices[base + 0];
        i1 = m_Indices[base + 1];
        i2 = m_Indices[base + 2];
        return i0 < m_Vertices.size() && i1 < m_Vertices.size() && i2 < m_Vertices.size();
    }

    glm::vec2 ReferenceGeometry::InterpolateUV(u32 triangleIndex, f32 u, f32 v) const
    {
        u32 i0, i1, i2;
        if (!GetTriangleVertices(triangleIndex, i0, i1, i2))
            return glm::vec2(0.0f);
        const f32 w = 1.0f - u - v;
        return m_Vertices[i0].TexCoord * w + m_Vertices[i1].TexCoord * u + m_Vertices[i2].TexCoord * v;
    }

    glm::vec3 ReferenceScene::ApplyNormalMap(const glm::vec3& n, const glm::vec3& p0, const glm::vec3& p1,
                                             const glm::vec3& p2, const glm::vec2& uv0, const glm::vec2& uv1,
                                             const glm::vec2& uv2, const glm::vec2& sampledXY, f32 normalScale)
    {
        const glm::vec3 e1 = p1 - p0;
        const glm::vec3 e2 = p2 - p0;
        const glm::vec2 d1 = uv1 - uv0;
        const glm::vec2 d2 = uv2 - uv0;
        const f32 det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f)
            return n;
        const f32 inv = 1.0f / det;
        glm::vec3 t = (e1 * d2.y - e2 * d1.y) * inv;
        const glm::vec3 bRaw = (e2 * d1.x - e1 * d2.x) * inv;
        t -= n * glm::dot(n, t);
        const f32 tLen = glm::length(t);
        if (!(tLen > 1e-12f))
            return n;
        t /= tLen;
        const glm::vec3 nCrossT = glm::cross(n, t);
        const glm::vec3 b = glm::dot(nCrossT, bRaw) < 0.0f ? -nCrossT : nCrossT;
        // PBRCommon.glsl's decodeTangentNormal.
        glm::vec2 nxy = sampledXY * 2.0f - 1.0f;
        nxy *= normalScale;
        const f32 nz = std::sqrt(std::max(0.0f, 1.0f - std::min(1.0f, glm::dot(nxy, nxy))));
        const glm::vec3 result = t * nxy.x + b * nxy.y + n * nz;
        const f32 len = glm::length(result);
        return (len > 1e-12f) ? result / len : n;
    }

    glm::vec3 ReferenceGeometry::InterpolateNormal(u32 triangleIndex, f32 u, f32 v) const
    {
        const sizet base = static_cast<sizet>(triangleIndex) * 3;
        if (base + 2 >= m_Indices.size())
            return glm::vec3(0.0f, 1.0f, 0.0f);

        const u32 i0 = m_Indices[base + 0];
        const u32 i1 = m_Indices[base + 1];
        const u32 i2 = m_Indices[base + 2];
        if (i0 >= m_Vertices.size() || i1 >= m_Vertices.size() || i2 >= m_Vertices.size())
            return glm::vec3(0.0f, 1.0f, 0.0f);

        const f32 w = 1.0f - u - v;
        const glm::vec3 n = m_Vertices[i0].Normal * w + m_Vertices[i1].Normal * u + m_Vertices[i2].Normal * v;

        // `!(x > eps)` so a NaN also takes the fallback — same guard shape as
        // PBRCommon.glsl's sanitizeSurfaceNormal, and for the same reason.
        const f32 lengthSq = glm::dot(n, n);
        if (!(lengthSq > 1e-20f))
        {
            const glm::vec3 e1 = m_Vertices[i1].Position - m_Vertices[i0].Position;
            const glm::vec3 e2 = m_Vertices[i2].Position - m_Vertices[i0].Position;
            const glm::vec3 geometric = glm::cross(e1, e2);
            const f32 geometricLengthSq = glm::dot(geometric, geometric);
            if (!(geometricLengthSq > 1e-20f))
                return glm::vec3(0.0f, 1.0f, 0.0f);
            return geometric * glm::inversesqrt(geometricLengthSq);
        }

        return n * glm::inversesqrt(lengthSq);
    }

    // =========================================================================
    // ReferenceScene — construction
    // =========================================================================

    u32 ReferenceScene::AddMaterial(const ReferenceMaterial& material)
    {
        m_Materials.push_back(material);
        m_Built = false;
        return static_cast<u32>(m_Materials.size() - 1);
    }

    u32 ReferenceScene::AddGeometry(std::vector<Vertex> vertices, std::vector<u32> indices)
    {
        m_Geometries.push_back(std::make_unique<ReferenceGeometry>(std::move(vertices), std::move(indices)));
        m_Built = false;
        return static_cast<u32>(m_Geometries.size() - 1);
    }

    namespace
    {
        // A masked triangle the ray passes through is skipped by re-casting
        // from just past it; the bound stops a pathological mesh (thousands
        // of coplanar cut-out layers) from looping.
        constexpr u32 kMaxMaskedRecasts = 64u;
        // The advance past a rejected triangle: an absolute floor plus a
        // relative term, because an absolute 1e-5 is below half an ulp of a
        // local distance past ~170 and would re-find the same triangle forever.
        constexpr f32 kMaskedRecastEpsilon = 1e-5f;
        constexpr f32 kMaskedRecastRelative = 1e-6f;

        [[nodiscard]] f32 MaskedRecastStart(f32 distance) noexcept
        {
            return distance + std::max(kMaskedRecastEpsilon, distance * kMaskedRecastRelative);
        }
    } // namespace

    u32 ReferenceScene::AddQuadGeometry(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3)
    {
        glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
        const f32 lengthSq = glm::dot(normal, normal);
        normal = (lengthSq > 1e-20f) ? normal * glm::inversesqrt(lengthSq) : glm::vec3(0.0f, 1.0f, 0.0f);

        std::vector<Vertex> vertices = {
            Vertex(p0, normal, glm::vec2(0.0f, 0.0f)),
            Vertex(p1, normal, glm::vec2(1.0f, 0.0f)),
            Vertex(p2, normal, glm::vec2(1.0f, 1.0f)),
            Vertex(p3, normal, glm::vec2(0.0f, 1.0f))
        };
        std::vector<u32> indices = { 0, 1, 2, 0, 2, 3 };
        return AddGeometry(std::move(vertices), std::move(indices));
    }

    u32 ReferenceScene::AddInstance(u32 geometryIndex, const glm::mat4& transform, u32 materialIndex)
    {
        constexpr u32 kInvalid = std::numeric_limits<u32>::max();

        if (geometryIndex >= m_Geometries.size())
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: geometry index {} out of range ({} geometries)",
                           geometryIndex, m_Geometries.size());
            return kInvalid;
        }
        if (materialIndex >= m_Materials.size())
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: material index {} out of range ({} materials)",
                           materialIndex, m_Materials.size());
            return kInvalid;
        }

        // Reject anything that is not rigid + uniform scale. The traversal
        // converts a running world-space TMax into instance-local units by a
        // single scalar; under non-uniform scale no such scalar exists, and the
        // resulting hit distances would be wrong in a way that still produces a
        // perfectly plausible image. Fail loudly at construction instead.
        const glm::vec3 axisX(transform[0]);
        const glm::vec3 axisY(transform[1]);
        const glm::vec3 axisZ(transform[2]);
        const f32 scaleX = glm::length(axisX);
        const f32 scaleY = glm::length(axisY);
        const f32 scaleZ = glm::length(axisZ);
        const f32 maxScale = std::max({ scaleX, scaleY, scaleZ });
        const f32 minScale = std::min({ scaleX, scaleY, scaleZ });
        if (!(minScale > 1e-6f) || (maxScale - minScale) > 1e-4f * maxScale)
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: transform must be rigid + uniform scale "
                           "(got per-axis scales {}, {}, {}); non-uniform scale would silently corrupt hit distances",
                           scaleX, scaleY, scaleZ);
            return kInvalid;
        }

        // A MIRRORED basis passes the test above — glm::length is never negative,
        // so a reflected axis looks like a perfectly uniform scale. It is not
        // harmless: a reflection flips triangle winding, and both places this
        // scene derives an outward direction from winding invert with it —
        // `BuildEmissiveList`'s cross(V1-V0, V2-V0) and `IntersectInstance`'s
        // geometric normal. A one-sided emitter would then emit from its back
        // face and the room would render black, which reads as an integrator
        // bug rather than a scene-setup one. Reject it as loudly as the
        // non-uniform case.
        if (glm::dot(glm::cross(axisX, axisY), axisZ) <= 0.0f)
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: transform mirrors the basis (non-positive determinant); "
                           "this flips triangle winding and inverts every geometric normal");
            return kInvalid;
        }

        ReferenceInstance instance;
        instance.GeometryIndex = geometryIndex;
        instance.MaterialIndex = materialIndex;
        instance.Transform = transform;
        instance.InverseTransform = glm::inverse(transform);
        instance.NormalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
        instance.UniformScale = maxScale;
        instance.WorldBounds = m_Geometries[geometryIndex]->GetLocalBounds().Transform(transform);

        if (!IsFinite(instance.WorldBounds))
        {
            OLO_CORE_ERROR("ReferenceScene::AddInstance: transform produced a non-finite world AABB");
            return kInvalid;
        }

        m_Instances.push_back(instance);
        m_Built = false;
        return static_cast<u32>(m_Instances.size() - 1);
    }

    void ReferenceScene::AddLight(const ReferenceLight& light)
    {
        m_Lights.push_back(light);
        m_Built = false;
    }

    const ReferenceMaterial& ReferenceScene::GetMaterial(u32 index) const
    {
        static const ReferenceMaterial s_Fallback{};
        if (index >= m_Materials.size())
            return s_Fallback;
        return m_Materials[index];
    }

    // =========================================================================
    // Build
    // =========================================================================

    void ReferenceScene::Build()
    {
        BuildTLAS();
        BuildEmissiveList();
        m_Built = true;
    }

    void ReferenceScene::BuildTLAS()
    {
        m_TLASNodes.clear();
        m_TLASInstanceRefs.clear();
        m_InstanceCentroids.clear();
        m_WorldBounds = BoundingBox(glm::vec3(0.0f), glm::vec3(0.0f));

        if (m_Instances.empty())
            return;

        m_TLASInstanceRefs.resize(m_Instances.size());
        std::iota(m_TLASInstanceRefs.begin(), m_TLASInstanceRefs.end(), 0u);

        m_InstanceCentroids.reserve(m_Instances.size());
        for (const ReferenceInstance& instance : m_Instances)
            m_InstanceCentroids.push_back(instance.WorldBounds.GetCenter());

        // Worst case for a binary tree over N leaves with >= 1 leaf each.
        m_TLASNodes.reserve(m_Instances.size() * 2);
        TLASNode& root = m_TLASNodes.emplace_back();
        root.LeftFirst = 0;
        root.Count = static_cast<u32>(m_TLASInstanceRefs.size());
        UpdateTLASNodeBounds(0);
        SubdivideTLAS(0, 0);

        m_WorldBounds = m_TLASNodes[0].Bounds;
    }

    void ReferenceScene::UpdateTLASNodeBounds(u32 nodeIndex)
    {
        TLASNode& node = m_TLASNodes[nodeIndex];
        glm::vec3 boundsMin(std::numeric_limits<f32>::max());
        glm::vec3 boundsMax(std::numeric_limits<f32>::lowest());
        for (u32 i = 0; i < node.Count; ++i)
        {
            const BoundingBox& instanceBounds = m_Instances[m_TLASInstanceRefs[node.LeftFirst + i]].WorldBounds;
            boundsMin = glm::min(boundsMin, instanceBounds.Min);
            boundsMax = glm::max(boundsMax, instanceBounds.Max);
        }
        node.Bounds = BoundingBox(boundsMin, boundsMax);
    }

    void ReferenceScene::SubdivideTLAS(u32 nodeIndex, u32 depth)
    {
        // Iterative to keep the recursion depth off the C stack, mirroring
        // BoundingVolumeHierarchy::Subdivide.
        struct Work
        {
            u32 NodeIndex;
            u32 Depth;
        };
        std::vector<Work> stack;
        stack.push_back({ nodeIndex, depth });

        while (!stack.empty())
        {
            const Work work = stack.back();
            stack.pop_back();

            TLASNode& node = m_TLASNodes[work.NodeIndex];
            if (node.Count <= s_MaxLeafInstances || work.Depth >= s_MaxDepth)
                continue;

            // Split on the largest axis of the CENTROID bounds at the spatial
            // midpoint, with an object-median fallback when the midpoint leaves
            // one side empty (identical policy to the bottom-level BVH).
            glm::vec3 centroidMin(std::numeric_limits<f32>::max());
            glm::vec3 centroidMax(std::numeric_limits<f32>::lowest());
            for (u32 i = 0; i < node.Count; ++i)
            {
                const glm::vec3& centroid = m_InstanceCentroids[m_TLASInstanceRefs[node.LeftFirst + i]];
                centroidMin = glm::min(centroidMin, centroid);
                centroidMax = glm::max(centroidMax, centroid);
            }

            const glm::vec3 extent = centroidMax - centroidMin;
            glm::length_t axis = 0;
            if (extent.y > extent[axis])
                axis = 1;
            if (extent.z > extent[axis])
                axis = 2;

            const auto first = m_TLASInstanceRefs.begin() + node.LeftFirst;
            const auto last = first + node.Count;

            const f32 splitPos = centroidMin[axis] + extent[axis] * 0.5f;
            auto middle = std::partition(first, last, [this, axis, splitPos](u32 instanceIndex)
                                         { return m_InstanceCentroids[instanceIndex][axis] < splitPos; });

            auto leftCount = static_cast<u32>(std::distance(first, middle));
            if (leftCount == 0 || leftCount == node.Count)
            {
                middle = first + node.Count / 2;
                std::nth_element(first, middle, last, [this, axis](u32 a, u32 b)
                                 { return m_InstanceCentroids[a][axis] < m_InstanceCentroids[b][axis]; });
                leftCount = node.Count / 2;
                if (leftCount == 0)
                    continue;
            }

            const u32 leftFirst = node.LeftFirst;
            const u32 totalCount = node.Count;
            const auto leftChild = static_cast<u32>(m_TLASNodes.size());

            m_TLASNodes.emplace_back();
            m_TLASNodes.emplace_back();

            // `node` may dangle after the emplaces reallocated the vector.
            m_TLASNodes[work.NodeIndex].LeftFirst = leftChild;
            m_TLASNodes[work.NodeIndex].Count = 0;

            m_TLASNodes[leftChild].LeftFirst = leftFirst;
            m_TLASNodes[leftChild].Count = leftCount;
            m_TLASNodes[leftChild + 1].LeftFirst = leftFirst + leftCount;
            m_TLASNodes[leftChild + 1].Count = totalCount - leftCount;

            UpdateTLASNodeBounds(leftChild);
            UpdateTLASNodeBounds(leftChild + 1);

            stack.push_back({ leftChild, work.Depth + 1 });
            stack.push_back({ leftChild + 1, work.Depth + 1 });
        }
    }

    void ReferenceScene::BuildEmissiveList()
    {
        m_EmissiveTriangles.clear();
        m_EmissiveAreaCdf.clear();
        m_TotalEmissiveArea = 0.0f;

        for (u32 instanceIndex = 0; instanceIndex < static_cast<u32>(m_Instances.size()); ++instanceIndex)
        {
            const ReferenceInstance& instance = m_Instances[instanceIndex];
            const ReferenceMaterial& material = GetMaterial(instance.MaterialIndex);
            if (!(std::max({ material.Emissive.x, material.Emissive.y, material.Emissive.z }) > 0.0f))
                continue;

            const ReferenceGeometry& geometry = *m_Geometries[instance.GeometryIndex];
            const std::vector<Vertex>& vertices = geometry.GetVertices();
            const std::vector<u32>& indices = geometry.GetIndices();

            for (sizet triangle = 0; triangle + 2 < indices.size(); triangle += 3)
            {
                const u32 i0 = indices[triangle + 0];
                const u32 i1 = indices[triangle + 1];
                const u32 i2 = indices[triangle + 2];
                if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                    continue;

                EmissiveTriangle emitter;
                emitter.V0 = glm::vec3(instance.Transform * glm::vec4(vertices[i0].Position, 1.0f));
                emitter.V1 = glm::vec3(instance.Transform * glm::vec4(vertices[i1].Position, 1.0f));
                emitter.V2 = glm::vec3(instance.Transform * glm::vec4(vertices[i2].Position, 1.0f));

                const glm::vec3 cross = glm::cross(emitter.V1 - emitter.V0, emitter.V2 - emitter.V0);
                const f32 crossLength = glm::length(cross);
                if (!(crossLength > 1e-12f))
                    continue; // degenerate triangle carries no light

                emitter.Area = 0.5f * crossLength;
                emitter.Normal = cross / crossLength;
                emitter.Uv0 = vertices[i0].TexCoord;
                emitter.Uv1 = vertices[i1].TexCoord;
                emitter.Uv2 = vertices[i2].TexCoord;
                emitter.InstanceIndex = instanceIndex;
                emitter.MaterialIndex = instance.MaterialIndex;
                emitter.TriangleIndex = static_cast<u32>(triangle / 3);

                m_TotalEmissiveArea += emitter.Area;
                m_EmissiveTriangles.push_back(emitter);
                m_EmissiveAreaCdf.push_back(m_TotalEmissiveArea);
            }
        }

        // Normalize the running sums into a CDF ending exactly at 1.
        if (m_TotalEmissiveArea > 0.0f)
        {
            for (f32& value : m_EmissiveAreaCdf)
                value /= m_TotalEmissiveArea;
            m_EmissiveAreaCdf.back() = 1.0f;
        }
    }

    // =========================================================================
    // Queries
    // =========================================================================

    bool ReferenceScene::IntersectInstance(u32 instanceIndex, const Ray& ray, SurfaceInteraction& outHit) const
    {
        const ReferenceInstance& instance = m_Instances[instanceIndex];
        const ReferenceGeometry& geometry = *m_Geometries[instance.GeometryIndex];

        const glm::vec3 localOrigin = glm::vec3(instance.InverseTransform * glm::vec4(ray.Origin, 1.0f));
        const glm::vec3 localDirRaw = glm::vec3(instance.InverseTransform * glm::vec4(ray.Direction, 0.0f));
        const f32 localDirLength = glm::length(localDirRaw);
        if (!(localDirLength > 0.0f))
            return false;
        const glm::vec3 localDir = localDirRaw / localDirLength;

        // A world distance t maps to a local distance t * localDirLength
        // (== t / UniformScale). Both interval ends convert the same way.
        Ray localRay;
        localRay.Origin = localOrigin;
        localRay.Direction = localDir;
        localRay.TMin = ray.TMin * localDirLength;
        localRay.TMax = (ray.TMax >= std::numeric_limits<f32>::max() * 0.5f)
                            ? ray.TMax
                            : ray.TMax * localDirLength;

        const ReferenceMaterial& material = GetMaterial(instance.MaterialIndex);

        // A masked material is not a hit where its alpha falls below the
        // cutoff: the ray continues past that triangle. The BVH returns the
        // closest hit only, so a rejected one is skipped by re-casting from
        // just beyond it — the CPU twin of the ray query's candidate loop.
        RayHit localHit;
        for (u32 attempt = 0;; ++attempt)
        {
            if (!geometry.GetBVH().CastRay(localRay, localHit))
                return false;
            if (!material.AlphaMask)
                break;
            // Past the bound the surface is treated as passed through, never
            // as a hit whose alpha was not tested.
            if (attempt >= kMaxMaskedRecasts)
                return false;
            f32 alpha = material.BaseAlpha;
            if (material.AlbedoMap)
                alpha *= material.AlbedoMap->SampleBilinear(geometry.InterpolateUV(localHit.TriangleIndex, localHit.U, localHit.V)).a;
            if (alpha >= material.AlphaCutoff)
                break;
            localRay.TMin = MaskedRecastStart(localHit.Distance);
            if (localRay.TMin >= localRay.TMax)
                return false;
        }

        const f32 worldDistance = localHit.Distance / localDirLength;

        outHit.Hit = true;
        outHit.Distance = worldDistance;
        outHit.Position = ray.Origin + ray.Direction * worldDistance;
        outHit.InstanceIndex = instanceIndex;
        outHit.MaterialIndex = instance.MaterialIndex;
        outHit.TriangleIndex = localHit.TriangleIndex;
        outHit.FrontFace = localHit.FrontFace;
        outHit.Uv = geometry.InterpolateUV(localHit.TriangleIndex, localHit.U, localHit.V);

        // RayHit::Normal is flipped to oppose the ray. Undo that so the caller
        // sees the true winding orientation (an emitter's front/back test needs
        // it, and the integrator flips it itself where it wants a shading
        // frame).
        const glm::vec3 localGeometric = localHit.FrontFace ? localHit.Normal : -localHit.Normal;
        const glm::vec3 localShading = geometry.InterpolateNormal(localHit.TriangleIndex, localHit.U, localHit.V);

        outHit.GeometricNormal = glm::normalize(instance.NormalMatrix * localGeometric);
        glm::vec3 shadingNormal = instance.NormalMatrix * localShading;
        const f32 shadingLengthSq = glm::dot(shadingNormal, shadingNormal);
        outHit.ShadingNormal = (shadingLengthSq > 1e-20f) ? shadingNormal * glm::inversesqrt(shadingLengthSq)
                                                          : outHit.GeometricNormal;

        // A shading normal that disagrees with the geometric one about which
        // side we are on produces black terminator artefacts and, worse, lets
        // NEE and BSDF sampling disagree about the hemisphere. Snap it.
        if (glm::dot(outHit.ShadingNormal, outHit.GeometricNormal) < 0.0f)
            outHit.ShadingNormal = outHit.GeometricNormal;

        // The normal map, in the triangle's world-space UV tangent frame —
        // after the snap and re-snapped, in the GPU's order.
        if (material.NormalMap)
        {
            u32 i0, i1, i2;
            if (geometry.GetTriangleVertices(localHit.TriangleIndex, i0, i1, i2))
            {
                const auto& vertices = geometry.GetVertices();
                const glm::vec3 p0 = glm::vec3(instance.Transform * glm::vec4(vertices[i0].Position, 1.0f));
                const glm::vec3 p1 = glm::vec3(instance.Transform * glm::vec4(vertices[i1].Position, 1.0f));
                const glm::vec3 p2 = glm::vec3(instance.Transform * glm::vec4(vertices[i2].Position, 1.0f));
                const glm::vec2 sampled = glm::vec2(material.NormalMap->SampleBilinear(outHit.Uv));
                outHit.ShadingNormal =
                    ApplyNormalMap(outHit.ShadingNormal, p0, p1, p2, vertices[i0].TexCoord, vertices[i1].TexCoord,
                                   vertices[i2].TexCoord, sampled, material.NormalScale);
                if (glm::dot(outHit.ShadingNormal, outHit.GeometricNormal) < 0.0f)
                    outHit.ShadingNormal = outHit.GeometricNormal;
            }
        }

        return true;
    }

    ReferenceMaterial ReferenceScene::ResolveMaterial(const SurfaceInteraction& hit) const
    {
        ReferenceMaterial material = GetMaterial(hit.MaterialIndex);
        if (material.AlbedoMap)
            material.BaseColor *= glm::vec3(material.AlbedoMap->SampleBilinear(hit.Uv));
        if (material.MetallicRoughnessMap)
        {
            const glm::vec4 metallicRoughness = material.MetallicRoughnessMap->SampleBilinear(hit.Uv);
            material.Metallic *= metallicRoughness.b;
            material.Roughness *= metallicRoughness.g;
        }
        if (material.EmissiveMap)
            material.Emissive *= glm::vec3(material.EmissiveMap->SampleBilinear(hit.Uv));
        return material;
    }

    bool ReferenceScene::OccludedInstance(u32 instanceIndex, const Ray& ray) const
    {
        const ReferenceInstance& instance = m_Instances[instanceIndex];
        const ReferenceGeometry& geometry = *m_Geometries[instance.GeometryIndex];

        const glm::vec3 localOrigin = glm::vec3(instance.InverseTransform * glm::vec4(ray.Origin, 1.0f));
        const glm::vec3 localDirRaw = glm::vec3(instance.InverseTransform * glm::vec4(ray.Direction, 0.0f));
        const f32 localDirLength = glm::length(localDirRaw);
        if (!(localDirLength > 0.0f))
            return false;

        Ray localRay;
        localRay.Origin = localOrigin;
        localRay.Direction = localDirRaw / localDirLength;
        localRay.TMin = ray.TMin * localDirLength;
        localRay.TMax = ray.TMax * localDirLength;

        const ReferenceMaterial& material = GetMaterial(instance.MaterialIndex);
        if (!material.AlphaMask)
            return geometry.GetBVH().CastRayAny(localRay);

        // Masked: an occluder is the first triangle whose alpha passes. Past
        // the bound the surface is treated as passed through.
        for (u32 attempt = 0; attempt < kMaxMaskedRecasts; ++attempt)
        {
            RayHit localHit;
            if (!geometry.GetBVH().CastRay(localRay, localHit))
                return false;
            f32 alpha = material.BaseAlpha;
            if (material.AlbedoMap)
                alpha *= material.AlbedoMap->SampleBilinear(geometry.InterpolateUV(localHit.TriangleIndex, localHit.U, localHit.V)).a;
            if (alpha >= material.AlphaCutoff)
                return true;
            localRay.TMin = MaskedRecastStart(localHit.Distance);
            if (localRay.TMin >= localRay.TMax)
                return false;
        }
        return false;
    }

    bool ReferenceScene::Intersect(const Ray& ray, SurfaceInteraction& outHit) const
    {
        // m_Built goes false on any Add*, so this catches BOTH "never built"
        // and "mutated after Build()" — the latter would otherwise traverse a
        // stale TLAS that simply does not contain the new geometry, which is
        // invisible in the output.
        OLO_CORE_ASSERT(m_Built, "ReferenceScene::Intersect on an unbuilt or stale scene — call Build() after the last Add*()");
        outHit = SurfaceInteraction{};
        if (m_TLASNodes.empty())
            return false;

        const glm::vec3 invDir(1.0f / ray.Direction.x, 1.0f / ray.Direction.y, 1.0f / ray.Direction.z);

        Ray working = ray;
        bool anyHit = false;

        u32 stack[s_TraversalStackSize];
        u32 stackSize = 0;
        stack[stackSize++] = 0;

        while (stackSize > 0)
        {
            const u32 nodeIndex = stack[--stackSize];
            const TLASNode& node = m_TLASNodes[nodeIndex];

            f32 tNear = 0.0f;
            if (!RayIntersect::RayAABB(working.Origin, invDir, node.Bounds.Min, node.Bounds.Max,
                                       working.TMin, working.TMax, tNear))
                continue;

            if (node.IsLeaf())
            {
                for (u32 i = 0; i < node.Count; ++i)
                {
                    const u32 instanceIndex = m_TLASInstanceRefs[node.LeftFirst + i];
                    SurfaceInteraction candidate;
                    if (IntersectInstance(instanceIndex, working, candidate))
                    {
                        outHit = candidate;
                        anyHit = true;
                        // Tighten the search interval so farther instances and
                        // whole subtrees prune out.
                        working.TMax = candidate.Distance;
                    }
                }
                continue;
            }

            // Push both children; the ordering does not affect correctness
            // because the interval is tightened on every accepted hit. The push
            // is UNCONDITIONAL: a capacity test that silently skipped the push
            // would drop a whole subtree and produce a plausible, wrong image.
            // The static_assert on s_TraversalStackSize (ReferenceScene.h) is
            // what makes overflow impossible; this asserts it at runtime too.
            OLO_CORE_ASSERT(stackSize + 2 <= s_TraversalStackSize, "TLAS traversal stack overflow");
            stack[stackSize++] = node.LeftFirst;
            stack[stackSize++] = node.LeftFirst + 1;
        }

        return anyHit;
    }

    bool ReferenceScene::IsOccluded(const glm::vec3& from, const glm::vec3& to, f32 epsilon) const
    {
        OLO_CORE_ASSERT(m_Built, "ReferenceScene::IsOccluded on an unbuilt or stale scene — call Build() after the last Add*()");
        if (m_TLASNodes.empty())
            return false;

        const glm::vec3 delta = to - from;
        const f32 distance = glm::length(delta);
        if (!(distance > 2.0f * epsilon))
            return false;

        Ray ray;
        ray.Origin = from;
        ray.Direction = delta / distance;
        ray.TMin = epsilon;
        // Inset the far end so the light sample's own surface is not counted
        // as its own occluder.
        ray.TMax = distance - epsilon;

        const glm::vec3 invDir(1.0f / ray.Direction.x, 1.0f / ray.Direction.y, 1.0f / ray.Direction.z);

        u32 stack[s_TraversalStackSize];
        u32 stackSize = 0;
        stack[stackSize++] = 0;

        while (stackSize > 0)
        {
            const u32 nodeIndex = stack[--stackSize];
            const TLASNode& node = m_TLASNodes[nodeIndex];

            f32 tNear = 0.0f;
            if (!RayIntersect::RayAABB(ray.Origin, invDir, node.Bounds.Min, node.Bounds.Max,
                                       ray.TMin, ray.TMax, tNear))
                continue;

            if (node.IsLeaf())
            {
                for (u32 i = 0; i < node.Count; ++i)
                {
                    if (OccludedInstance(m_TLASInstanceRefs[node.LeftFirst + i], ray))
                        return true;
                }
                continue;
            }

            OLO_CORE_ASSERT(stackSize + 2 <= s_TraversalStackSize, "TLAS traversal stack overflow");
            stack[stackSize++] = node.LeftFirst;
            stack[stackSize++] = node.LeftFirst + 1;
        }

        return false;
    }

    // =========================================================================
    // Emissive sampling
    // =========================================================================

    f32 ReferenceScene::EmissivePdfArea() const
    {
        if (!(m_TotalEmissiveArea > 0.0f))
            return 0.0f;
        return 1.0f / m_TotalEmissiveArea;
    }

    bool ReferenceScene::SampleEmissive(f32 xiSelect, const glm::vec2& xiPoint, EmissiveSample& outSample) const
    {
        if (m_EmissiveTriangles.empty() || !(m_TotalEmissiveArea > 0.0f))
            return false;

        // Area-proportional triangle selection. Uniform-over-area (rather than
        // uniform-over-triangles) is what makes the density constant, which in
        // turn is what lets EmissivePdfArea() be a single number the MIS side
        // can reuse without re-deriving it.
        const auto it = std::lower_bound(m_EmissiveAreaCdf.begin(), m_EmissiveAreaCdf.end(), xiSelect);
        sizet triangleIndex = static_cast<sizet>(std::distance(m_EmissiveAreaCdf.begin(), it));
        if (triangleIndex >= m_EmissiveTriangles.size())
            triangleIndex = m_EmissiveTriangles.size() - 1;

        const EmissiveTriangle& emitter = m_EmissiveTriangles[triangleIndex];

        // Uniform barycentric point (Turk's square-root warp).
        const f32 sqrtU = std::sqrt(std::clamp(xiPoint.x, 0.0f, 1.0f));
        const f32 b0 = 1.0f - sqrtU;
        const f32 b1 = std::clamp(xiPoint.y, 0.0f, 1.0f) * sqrtU;
        const f32 b2 = 1.0f - b0 - b1;

        const ReferenceMaterial& material = GetMaterial(emitter.MaterialIndex);
        outSample.Position = emitter.V0 * b0 + emitter.V1 * b1 + emitter.V2 * b2;
        outSample.Normal = emitter.Normal;
        outSample.Radiance = material.Emissive;
        // The emissive map at the sampled point, so NEE and the emitter-hit
        // path see one radiance and MIS weights the same integrand twice.
        if (material.EmissiveMap)
        {
            const glm::vec2 uv = emitter.Uv0 * b0 + emitter.Uv1 * b1 + emitter.Uv2 * b2;
            outSample.Radiance *= glm::vec3(material.EmissiveMap->SampleBilinear(uv));
        }
        outSample.PdfArea = EmissivePdfArea();
        outSample.TwoSided = material.TwoSidedEmission;
        return true;
    }
} // namespace OloEngine::PathTracing
