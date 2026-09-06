// OLO_TEST_LAYER: integration
//
// Visibility-buffer evidence for the software raster's sub-sample-miss reject
// (issue #712).
//
// The reject skips a micro-triangle whose window-space bounding box contains no
// pixel CENTRE, and tightens the surviving triangles' scan bounds to the centres
// they can cover. Its failure mode is silent and specific: half a pixel too
// aggressive and it stops rasterizing triangles that DO cover a sample, which
// shows up as scattered PINHOLES in the visibility buffer — single uncovered
// pixels with covered neighbours all round — and, after the resolve, as speckled
// background dots on a solid surface that no coverage-ratio test notices.
//
// So the contract pinned here is exactly that: over a dense micro-triangle
// subject rasterized entirely in software, from several angles, the visibility
// buffer has NO interior holes. A watertight mesh cannot produce one — the edge
// test accepts a zero edge function, so triangles sharing an edge both claim the
// pixels on it — and every hole-producing mechanism in the shader (the sub-sample
// reject, the bounding-box work cap, the near-plane guard) is a triangle-level
// drop that this counts directly.
//
// It doubles as the A/B instrument for the change: each capture logs a digest of
// the visibility buffer (covered count + FNV-1a over the depth words and over the
// payload words) on a `visbuffer-digest` line, so the same binary run against two
// versions of VirtualClusterRaster.comp — shaders are runtime assets — proves
// the buffer is bit-identical, or says exactly where it is not. The depth digest
// is the deterministic half; payloads can legitimately differ where two triangles
// tie at bit-identical depth, which the raster documents as a benign race.
//
// SKIPs cleanly with no GL 4.6 context (suite gate).

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/glm.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        // The visibility buffer's "nothing here" word, written by
        // VirtualMeshRegistry::EnsureVisbuffer (0xFFFFFFFF into every u32).
        constexpr u32 kEmptyWord = 0xFFFFFFFFu;

        // Icosphere, subdivided until its triangles are far smaller than a pixel
        // at the capture distance — the micropoly regime the reject targets.
        // (Same construction as VirtualGeometryVisualEvidenceTest's.)
        Ref<MeshSource> MakeIcosphereMeshSource(u32 subdivisions)
        {
            const f32 t = (1.0f + std::sqrt(5.0f)) * 0.5f;
            std::vector<glm::vec3> positions = {
                { -1.0f, t, 0.0f },
                { 1.0f, t, 0.0f },
                { -1.0f, -t, 0.0f },
                { 1.0f, -t, 0.0f },
                { 0.0f, -1.0f, t },
                { 0.0f, 1.0f, t },
                { 0.0f, -1.0f, -t },
                { 0.0f, 1.0f, -t },
                { t, 0.0f, -1.0f },
                { t, 0.0f, 1.0f },
                { -t, 0.0f, -1.0f },
                { -t, 0.0f, 1.0f },
            };
            for (auto& p : positions)
            {
                p = glm::normalize(p);
            }
            std::vector<u32> indices = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11,
                                         4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3,
                                         6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
            for (u32 s = 0; s < subdivisions; ++s)
            {
                std::map<std::pair<u32, u32>, u32> midpointCache;
                auto midpoint = [&](u32 a, u32 b) -> u32
                {
                    std::pair<u32, u32> const key = std::minmax(a, b);
                    if (auto it = midpointCache.find(key); it != midpointCache.end())
                    {
                        return it->second;
                    }
                    auto index = static_cast<u32>(positions.size());
                    positions.push_back(glm::normalize((positions[a] + positions[b]) * 0.5f));
                    midpointCache.emplace(key, index);
                    return index;
                };
                std::vector<u32> next;
                next.reserve(indices.size() * 4);
                for (sizet i = 0; i + 2 < indices.size(); i += 3)
                {
                    u32 const a = indices[i];
                    u32 const b = indices[i + 1];
                    u32 const c = indices[i + 2];
                    u32 const ab = midpoint(a, b);
                    u32 const bc = midpoint(b, c);
                    u32 const ca = midpoint(c, a);
                    for (u32 idx : { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca })
                    {
                        next.push_back(idx);
                    }
                }
                indices = std::move(next);
            }

            TArray<Vertex> vertices;
            vertices.Reserve(static_cast<i32>(positions.size()));
            constexpr f32 kPi = 3.14159265358979323846f;
            for (const glm::vec3& p : positions)
            {
                glm::vec2 const uv{ std::atan2(p.z, p.x) / (2.0f * kPi) + 0.5f,
                                    std::asin(std::clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f };
                vertices.Add(Vertex(p, p, uv));
            }
            TArray<u32> meshIndices;
            meshIndices.Reserve(static_cast<i32>(indices.size()));
            for (u32 const index : indices)
            {
                meshIndices.Add(index);
            }
            return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(meshIndices));
        }

        // One pixel of the visibility buffer, in the layout BOTH raster variants
        // write: the portable 2x32 path declares uvec2 {payload, depth} and the
        // single-pass path a uint64_t (depthBits << 32 | payload), which is the
        // same eight bytes on a little-endian device.
        struct VisPixel
        {
            u32 Payload{ kEmptyWord };
            u32 Depth{ kEmptyWord };

            [[nodiscard]] bool Covered() const
            {
                return Depth != kEmptyWord;
            }
        };

        struct VisDigest
        {
            u32 Covered{ 0 };
            u32 InteriorHoles{ 0 };
            u32 NonFiniteDepths{ 0 };
            u64 DepthHash{ 0 };
            u64 PayloadHash{ 0 };
        };

        u64 FnvAppend(u64 hash, u32 value)
        {
            hash ^= value;
            return hash * 1099511628211ull;
        }

        VisDigest Summarize(const std::vector<VisPixel>& pixels, u32 width, u32 height)
        {
            VisDigest digest;
            digest.DepthHash = 14695981039346656037ull;
            digest.PayloadHash = 14695981039346656037ull;
            for (const VisPixel& p : pixels)
            {
                digest.DepthHash = FnvAppend(digest.DepthHash, p.Depth);
                digest.PayloadHash = FnvAppend(digest.PayloadHash, p.Payload);
                if (!p.Covered())
                    continue;
                ++digest.Covered;
                const f32 depth = std::bit_cast<f32>(p.Depth);
                if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f)
                    ++digest.NonFiniteDepths;
            }

            // An uncovered pixel whose four neighbours are all covered. On a
            // watertight subject this is only ever a triangle the raster
            // dropped — which is the whole risk of a sub-sample reject.
            for (u32 y = 1; y + 1 < height; ++y)
            {
                for (u32 x = 1; x + 1 < width; ++x)
                {
                    const sizet i = static_cast<sizet>(y) * width + x;
                    if (pixels[i].Covered())
                        continue;
                    if (pixels[i - 1].Covered() && pixels[i + 1].Covered() &&
                        pixels[i - width].Covered() && pixels[i + width].Covered())
                    {
                        ++digest.InteriorHoles;
                    }
                }
            }
            return digest;
        }

        void WriteVisbufferPng(const std::string& name, const std::vector<VisPixel>& pixels, u32 width,
                               u32 height)
        {
            // Stretch the covered depth range over the ramp — a virtual mesh
            // occupies a sliver of [0,1] and a raw depth image reads as flat white.
            f32 minDepth = 1.0f;
            f32 maxDepth = 0.0f;
            for (const VisPixel& p : pixels)
            {
                if (!p.Covered())
                    continue;
                const f32 d = std::bit_cast<f32>(p.Depth);
                if (!std::isfinite(d))
                    continue;
                minDepth = std::min(minDepth, d);
                maxDepth = std::max(maxDepth, d);
            }
            const f32 span = std::max(maxDepth - minDepth, 1e-6f);

            std::vector<u8> rgba(static_cast<sizet>(width) * height * 4, 0);
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const sizet i = static_cast<sizet>(y) * width + x;
                    // The visibility buffer is indexed y * width + x in GL window
                    // space, whose y counts up from the BOTTOM (the raster derives
                    // it from ndc.y, and the resolve reads it back at
                    // gl_FragCoord.y). PNG rows run top-down, so flip — the same
                    // flip WriteEvidencePng does for a framebuffer readback.
                    const sizet o = (static_cast<sizet>(height - 1 - y) * width + x) * 4;
                    if (pixels[i].Covered())
                    {
                        const f32 d = std::bit_cast<f32>(pixels[i].Depth);
                        const f32 t = std::isfinite(d) ? std::clamp(1.0f - (d - minDepth) / span, 0.0f, 1.0f) : 0.0f;
                        const auto level = static_cast<u8>(t * 255.0f);
                        rgba[o + 0] = level;
                        rgba[o + 1] = level;
                        rgba[o + 2] = level;
                    }
                    else if (x > 0 && y > 0 && x + 1 < width && y + 1 < height &&
                             pixels[i - 1].Covered() && pixels[i + 1].Covered() &&
                             pixels[i - width].Covered() && pixels[i + width].Covered())
                    {
                        // Interior hole — the failure this test exists for, made
                        // visible in the evidence rather than only counted.
                        rgba[o + 0] = 255;
                    }
                    rgba[o + 3] = 255;
                }
            }

            namespace fs = std::filesystem;
            fs::create_directories("assets/tests/visual");
            const std::string path = "assets/tests/visual/" + name;
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               rgba.data(), static_cast<int>(width * 4));
            EXPECT_NE(wrote, 0) << "failed to write evidence PNG " << path;
        }
    } // namespace

    class VirtualRasterSubSampleEvidence : public RendererAttachedTest
    {
      public:
        void BuildScene() override
        {
            // AssetManager::AddMemoryOnlyAsset needs an active project + asset
            // manager; same throwaway mount as VirtualGeometryVisualEvidence.
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path const projectDir = OloEngine::Tests::TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: VirtualRasterSubSampleEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            {
                m_SunEntity = scene.CreateEntity("Sun");
                auto& dl = m_SunEntity.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.2f, -0.95f, 0.25f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 3.0f;
            }

            // The ONLY virtual mesh in the scene, so every covered visibility
            // -buffer pixel belongs to one convex silhouette and an uncovered
            // pixel with covered neighbours all round is unambiguously a hole.
            // 81920 triangles at subdivision 6: well under a pixel each at the
            // capture distances below.
            {
                auto meshSource = MakeIcosphereMeshSource(6);
                AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(meshSource);

                Entity sphere = scene.CreateEntity("VirtualSphere");
                auto& vm = sphere.AddComponent<VirtualMeshComponent>();
                vm.m_MeshSource = handle;
                vm.m_ErrorThresholdPixels = 1.0f;
                auto& mat = sphere.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                mat.m_Material.SetRoughnessFactor(0.6f);
                m_SphereEntity = sphere;
            }
        }

        void TearDown() override
        {
            VirtualMeshRegistry::Get().SetSwRasterMode(VirtualSwRasterMode::Auto);
            VirtualMeshRegistry::Get().SetForcePortableSwRaster(false);
            RendererAttachedTest::TearDown();
        }

        Entity m_SphereEntity;
        Entity m_SunEntity;

        // Reads the visibility buffer through a staging copy. Never straight off
        // m_VisbufferBuffer: it is GL_DYNAMIC_COPY on purpose (the GPU both
        // writes and reads it every frame) and a CPU read migrates it
        // VIDEO -> HOST permanently — the same trap VirtualMeshRegistry::
        // ReadFrameCullStats documents for the args buffer.
        bool ReadVisbuffer(std::vector<VisPixel>& out, u32& outWidth, u32& outHeight)
        {
            VirtualMeshRegistry& registry = VirtualMeshRegistry::Get();
            const Ref<StorageBuffer>& visbuffer = registry.GetVisbufferBuffer();
            outWidth = registry.GetVisbufferWidth();
            outHeight = registry.GetVisbufferHeight();
            if (!visbuffer || outWidth == 0 || outHeight == 0)
                return false;

            const auto pixelCount = static_cast<sizet>(outWidth) * outHeight;
            const auto bytes = static_cast<GLsizeiptr>(pixelCount * sizeof(VisPixel));
            if (static_cast<u32>(bytes) > visbuffer->GetSize())
                return false;

            GLuint staging = 0;
            ::glCreateBuffers(1, &staging);
            ::glNamedBufferStorage(staging, bytes, nullptr, GL_MAP_READ_BIT);
            // Order the raster's shader writes BEFORE the copy reads them. The
            // pass already barriers after its dispatches, but this readback is
            // outside the frame and must not rely on that.
            ::glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
            ::glCopyNamedBufferSubData(visbuffer->GetRendererID(), staging, 0, 0, bytes);
            out.assign(pixelCount, VisPixel{});
            ::glGetNamedBufferSubData(staging, 0, bytes, out.data());
            ::glDeleteBuffers(1, &staging);
            return true;
        }

        // One pose, rasterized entirely in software, summarized and written out.
        VisDigest CaptureSoftwareVisbuffer(const char* name, const glm::vec3& position, f32 yaw, f32 pitch)
        {
            VirtualMeshRegistry::Get().SetSwRasterMode(VirtualSwRasterMode::ForceSoftware);

            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, 3);

            std::vector<VisPixel> pixels;
            u32 width = 0;
            u32 height = 0;
            if (!ReadVisbuffer(pixels, width, height))
            {
                ADD_FAILURE() << "visibility-buffer readback unavailable for " << name;
                return {};
            }

            const VisDigest digest = Summarize(pixels, width, height);
            WriteVisbufferPng(std::string("VirtualRaster_Visbuffer_") + name + ".png", pixels, width, height);

            // The A/B line. Grep-stable on purpose: running this binary against
            // two versions of VirtualClusterRaster.comp and diffing these proves
            // (or disproves) a bit-identical visibility buffer without a golden.
            GTEST_LOG_(INFO) << "visbuffer-digest " << name << " size=" << width << "x" << height
                             << " covered=" << digest.Covered << " holes=" << digest.InteriorHoles
                             << " depthHash=" << digest.DepthHash << " payloadHash=" << digest.PayloadHash;
            return digest;
        }
    };

    // The contract: a dense micro-triangle subject rasterized entirely in
    // software leaves NO interior holes in the visibility buffer, from any angle.
    // A sub-sample reject that is half a pixel too aggressive fails here and
    // nowhere else in the suite.
    TEST_F(VirtualRasterSubSampleEvidence, SoftwareVisibilityBufferHasNoInteriorHoles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // Several angles, and two distances: the near pose gives triangles of
        // roughly a pixel (where the reject is most active and most dangerous),
        // the far pose puts several triangles inside one pixel.
        //
        // Yaw/pitch are RADIANS, yaw 0 looks toward -Z and +yaw toward +X, and
        // POSITIVE pitch tilts the view DOWN (EditorCamera::Focus says so;
        // WaterVisualEvidenceTest's pose comment claims the opposite and is
        // wrong). Each pose below aims at the origin, where the sphere is: the
        // oblique eye at (3, 1.4, 3) looks along (-X, -Z), which is yaw -pi/4,
        // and down by atan(1.4 / sqrt(3^2+3^2)) = 0.32. The `covered` floor
        // asserted below is what catches a pose that photographs empty space —
        // writing a PNG is not an assertion about what is in it.
        const std::array<Pose, 4> poses{ {
            { "Front", { 0.0f, 0.0f, 4.0f }, 0.0f, 0.0f },
            { "Above", { 0.0f, 3.2f, 2.6f }, 0.0f, 0.85f },
            { "Oblique", { 3.0f, 1.4f, 3.0f }, -0.785f, 0.32f },
            { "Far", { 0.0f, 0.0f, 11.0f }, 0.0f, 0.0f },
        } };

        u32 totalCovered = 0;
        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);
            const VisDigest digest = CaptureSoftwareVisbuffer(pose.Name, pose.Position, pose.Yaw, pose.Pitch);

            // Anti-vacuous: a pose that rendered nothing would pass every check
            // below trivially.
            EXPECT_GT(digest.Covered, 1500u)
                << pose.Name << ": the software rasterizer barely covered anything — the subject is not "
                << "in frame, or the SW path did not run";
            totalCovered += digest.Covered;

            EXPECT_EQ(digest.InteriorHoles, 0u)
                << pose.Name << ": " << digest.InteriorHoles
                << " interior hole(s) in the visibility buffer. A watertight mesh cannot produce one — the "
                << "raster dropped triangles that cover a sample point. See the red pixels in "
                << "assets/tests/visual/VirtualRaster_Visbuffer_" << pose.Name << ".png";

            EXPECT_EQ(digest.NonFiniteDepths, 0u)
                << pose.Name << ": " << digest.NonFiniteDepths
                << " covered pixel(s) hold a depth outside [0,1] or non-finite";
        }

        EXPECT_GT(totalCovered, 20000u);
    }
} // namespace OloEngine::Tests
