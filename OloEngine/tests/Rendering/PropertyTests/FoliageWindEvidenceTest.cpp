// OLO_TEST_LAYER: L8
// Full production raster pipeline: wind on/off, motion and pause across GL paths.
#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"
#include "RendererStateCheck.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <stb_image/stb_image_write.h>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#if defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif

namespace OloEngine::Tests
{
    namespace
    {
        // TEMP diagnostic (#1484 foliage OOM): log every GL storage allocation
        // estimated over 256 MiB, by swapping glad's entry points.
        constexpr unsigned long long kLargeAllocation = 256ull << 20;
        unsigned long long s_AllocationCalls = 0;
        unsigned long long s_PersistentBytes = 0;
        unsigned long long s_PersistentCount = 0;
        PFNGLTEXTURESTORAGE2DPROC s_RealStorage2D = nullptr;
        PFNGLTEXTURESTORAGE3DPROC s_RealStorage3D = nullptr;
        PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC s_RealStorage2DMs = nullptr;
        PFNGLNAMEDBUFFERSTORAGEPROC s_RealBufferStorage = nullptr;
        PFNGLNAMEDBUFFERDATAPROC s_RealBufferData = nullptr;
        void ReportLarge(const char* what, unsigned long long bytes, long long a, long long b, long long c, long long d,
                         unsigned fmt)
        {
            ++s_AllocationCalls;
            if (bytes < kLargeAllocation)
                return;
            std::fprintf(stdout, "[foliage-alloc] %s ~%llu MiB (%lld x %lld x %lld, levels/samples %lld, fmt 0x%x)\n", what,
                         bytes >> 20, a, b, c, d, fmt);
            std::fflush(stdout);
#if defined(__linux__)
            void* frames[24];
            const int n = ::backtrace(frames, 24);
            ::backtrace_symbols_fd(frames, n, STDOUT_FILENO);
#endif
        }
        void GLAD_API_PTR LogStorage2D(GLuint t, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h)
        {
            ReportLarge("TextureStorage2D", 16ull * static_cast<unsigned long long>(w) * h * 4 / 3, w, h, 1, levels, fmt);
            s_RealStorage2D(t, levels, fmt, w, h);
        }
        void GLAD_API_PTR LogStorage3D(GLuint t, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h, GLsizei dep)
        {
            ReportLarge("TextureStorage3D", 16ull * static_cast<unsigned long long>(w) * h * dep, w, h, dep, levels, fmt);
            s_RealStorage3D(t, levels, fmt, w, h, dep);
        }
        void GLAD_API_PTR LogStorage2DMs(GLuint t, GLsizei samples, GLenum fmt, GLsizei w, GLsizei h, GLboolean fixed)
        {
            ReportLarge("TextureStorage2DMultisample", 16ull * static_cast<unsigned long long>(w) * h * samples, w, h, 1,
                        samples, fmt);
            s_RealStorage2DMs(t, samples, fmt, w, h, fixed);
        }
        void GLAD_API_PTR LogBufferStorage(GLuint buf, GLsizeiptr size, const void* data, GLbitfield flags)
        {
            if ((flags & 0x40u) != 0u)
            {
                s_PersistentBytes += static_cast<unsigned long long>(size);
                ++s_PersistentCount;
                if (size >= (1 << 20))
                {
                    std::printf("[foliage-persist] buffer %u: %lld KiB flags 0x%x (total %llu MiB)\n", buf, static_cast<long long>(size) >> 10, flags, s_PersistentBytes >> 20);
                    std::fflush(stdout);
                }
            }
            ReportLarge("NamedBufferStorage", static_cast<unsigned long long>(size), size, 1, 1, 0, flags);
            s_RealBufferStorage(buf, size, data, flags);
        }
        void GLAD_API_PTR LogBufferData(GLuint buf, GLsizeiptr size, const void* data, GLenum usage)
        {
            ReportLarge("NamedBufferData", static_cast<unsigned long long>(size), size, 1, 1, 0, usage);
            s_RealBufferData(buf, size, data, usage);
        }
        PFNGLBUFFERDATAPROC s_RealBufferDataNonDsa = nullptr;
        PFNGLBUFFERSTORAGEPROC s_RealBufferStorageNonDsa = nullptr;
        PFNGLTEXIMAGE2DPROC s_RealTexImage2D = nullptr;
        PFNGLTEXIMAGE3DPROC s_RealTexImage3D = nullptr;
        PFNGLTEXSTORAGE2DPROC s_RealTexStorage2D = nullptr;
        PFNGLTEXSTORAGE3DPROC s_RealTexStorage3D = nullptr;
        PFNGLNAMEDRENDERBUFFERSTORAGEMULTISAMPLEPROC s_RealRbMs = nullptr;
        PFNGLNAMEDRENDERBUFFERSTORAGEPROC s_RealRb = nullptr;
        void GLAD_API_PTR LogBufferDataNonDsa(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
        {
            ReportLarge("BufferData", static_cast<unsigned long long>(size), size, 1, 1, 0, target);
            s_RealBufferDataNonDsa(target, size, data, usage);
        }
        void GLAD_API_PTR LogBufferStorageNonDsa(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
        {
            ReportLarge("BufferStorage", static_cast<unsigned long long>(size), size, 1, 1, 0, flags);
            s_RealBufferStorageNonDsa(target, size, data, flags);
        }
        void GLAD_API_PTR LogTexImage2D(GLenum target, GLint level, GLint fmt, GLsizei w, GLsizei h, GLint border,
                                        GLenum format, GLenum type, const void* px)
        {
            ReportLarge("TexImage2D", 16ull * static_cast<unsigned long long>(w) * h, w, h, 1, level, static_cast<unsigned>(fmt));
            s_RealTexImage2D(target, level, fmt, w, h, border, format, type, px);
        }
        void GLAD_API_PTR LogTexImage3D(GLenum target, GLint level, GLint fmt, GLsizei w, GLsizei h, GLsizei d, GLint border,
                                        GLenum format, GLenum type, const void* px)
        {
            ReportLarge("TexImage3D", 16ull * static_cast<unsigned long long>(w) * h * d, w, h, d, level,
                        static_cast<unsigned>(fmt));
            s_RealTexImage3D(target, level, fmt, w, h, d, border, format, type, px);
        }
        void GLAD_API_PTR LogTexStorage2D(GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h)
        {
            ReportLarge("TexStorage2D", 16ull * static_cast<unsigned long long>(w) * h * 4 / 3, w, h, 1, levels, fmt);
            s_RealTexStorage2D(target, levels, fmt, w, h);
        }
        void GLAD_API_PTR LogTexStorage3D(GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h, GLsizei d)
        {
            ReportLarge("TexStorage3D", 16ull * static_cast<unsigned long long>(w) * h * d, w, h, d, levels, fmt);
            s_RealTexStorage3D(target, levels, fmt, w, h, d);
        }
        void GLAD_API_PTR LogRbMs(GLuint rb, GLsizei samples, GLenum fmt, GLsizei w, GLsizei h)
        {
            ReportLarge("RenderbufferStorageMultisample", 16ull * static_cast<unsigned long long>(w) * h * (samples > 0 ? samples : 1),
                        w, h, 1, samples, fmt);
            s_RealRbMs(rb, samples, fmt, w, h);
        }
        void GLAD_API_PTR LogRb(GLuint rb, GLenum fmt, GLsizei w, GLsizei h)
        {
            ReportLarge("RenderbufferStorage", 16ull * static_cast<unsigned long long>(w) * h, w, h, 1, 0, fmt);
            s_RealRb(rb, fmt, w, h);
        }

        // Every live texture's level 0, largest first.
        void TextureCensus()
        {
            unsigned long long total = 0;
            unsigned long long count = 0;
            struct Row
            {
                unsigned long long Bytes;
                GLuint Id;
                GLint W, H, D, Samples, Format, Levels;
            };
            std::vector<Row> rows;
            for (GLuint id = 1; id < 60000; ++id)
            {
                if (!::glIsTexture(id))
                    continue;
                GLint w = 0, h = 0, d = 0, samples = 0, fmt = 0, levels = 0;
                ::glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_WIDTH, &w);
                ::glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_HEIGHT, &h);
                ::glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_DEPTH, &d);
                ::glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_SAMPLES, &samples);
                ::glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
                ::glGetTextureParameteriv(id, GL_TEXTURE_IMMUTABLE_LEVELS, &levels);
                const unsigned long long texels = static_cast<unsigned long long>(std::max(w, 0)) * std::max(h, 1) *
                                                  std::max(d, 1) * std::max(samples, 1);
                total += texels * 8ull;
                ++count;
                rows.push_back({ texels * 8ull, id, w, h, d, samples, fmt, levels });
            }
            while (::glGetError() != GL_NO_ERROR)
            {
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
                      { return a.Bytes > b.Bytes; });
            std::printf("[foliage-textures] %llu textures, ~%llu MiB at 8 B/texel; largest:", count, total >> 20);
            for (sizet i = 0; i < rows.size() && i < 8; ++i)
                std::printf(" #%u %dx%dx%d s%d l%d fmt 0x%x;", rows[i].Id, rows[i].W, rows[i].H, rows[i].D, rows[i].Samples,
                            rows[i].Levels, static_cast<unsigned>(rows[i].Format));
            std::printf("\n");
            std::fflush(stdout);
        }

        // Driver messages: radeonsi reports each compiled shader's statistics
        // (including scratch bytes per wave) and any allocation failure here.
        void GLAD_API_PTR LogDriverMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                           const GLchar* message, const void*)
        {
            const std::string text(message, length > 0 ? static_cast<sizet>(length) : std::strlen(message));
            const bool scratch = text.find("Scratch") != std::string::npos && text.find("Scratch: 0 ") == std::string::npos;
            const bool severe = severity == GL_DEBUG_SEVERITY_HIGH || type == GL_DEBUG_TYPE_ERROR;
            const bool memory = text.find("memory") != std::string::npos || text.find("Memory") != std::string::npos;
            if (!scratch && !severe && !memory)
                return;
            std::printf("[foliage-driver] src 0x%x type 0x%x id %u sev 0x%x: %s\n", source, type, id, severity,
                        text.substr(0, 600).c_str());
            std::fflush(stdout);
        }

        void InstallAllocationLog()
        {
            ::glEnable(GL_DEBUG_OUTPUT);
            ::glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            ::glDebugMessageCallback(LogDriverMessage, nullptr);
            ::glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
            if (s_RealStorage2D != nullptr)
                return;
            s_RealStorage2D = glad_glTextureStorage2D;
            s_RealStorage3D = glad_glTextureStorage3D;
            s_RealStorage2DMs = glad_glTextureStorage2DMultisample;
            s_RealBufferStorage = glad_glNamedBufferStorage;
            s_RealBufferData = glad_glNamedBufferData;
            glad_glTextureStorage2D = LogStorage2D;
            glad_glTextureStorage3D = LogStorage3D;
            glad_glTextureStorage2DMultisample = LogStorage2DMs;
            glad_glNamedBufferStorage = LogBufferStorage;
            glad_glNamedBufferData = LogBufferData;
            s_RealBufferDataNonDsa = glad_glBufferData;
            s_RealBufferStorageNonDsa = glad_glBufferStorage;
            s_RealTexImage2D = glad_glTexImage2D;
            s_RealTexImage3D = glad_glTexImage3D;
            s_RealTexStorage2D = glad_glTexStorage2D;
            s_RealTexStorage3D = glad_glTexStorage3D;
            s_RealRbMs = glad_glNamedRenderbufferStorageMultisample;
            s_RealRb = glad_glNamedRenderbufferStorage;
            glad_glBufferData = LogBufferDataNonDsa;
            glad_glBufferStorage = LogBufferStorageNonDsa;
            glad_glTexImage2D = LogTexImage2D;
            glad_glTexImage3D = LogTexImage3D;
            glad_glTexStorage2D = LogTexStorage2D;
            glad_glTexStorage3D = LogTexStorage3D;
            glad_glNamedRenderbufferStorageMultisample = LogRbMs;
            glad_glNamedRenderbufferStorage = LogRb;
        }

        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        // The Drift conifer, relative to OloEditor/ (the suite's working
        // directory) — the same path Woodland.olo and the impostor bake use.
        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";

        // The hand-over band this test authors. Wide enough that the near camera
        // sits well inside it and the far camera well outside, so neither
        // assertion depends on where exactly the dither lands.
        constexpr f32 kMeshFadeStart = 55.0f;
        constexpr f32 kMeshViewDistance = 70.0f;

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

    } // namespace

    class FoliageWindEvidenceTest : public RendererAttachedTest
    {
      protected:
        RendererState::Snapshot m_SavedState;

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            RendererState::Restore(m_SavedState);
        }

        void BuildScene() override
        {
            ASSERT_TRUE(RendererState::Capture(m_SavedState));
            auto& wind = Renderer3D::GetWindSettings();
            wind = WindSettings{};
            wind.Enabled = true;
            wind.Direction = glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f));
            wind.Speed = 8.0f;
            wind.GustStrength = 0.6f;
            wind.GustFrequency = 0.4f;
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            m_TerrainEntity = scene.CreateEntityWithUUID(UUID(1236), "Terrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 11;
                terrain.m_ProceduralResolution = 128;
                terrain.m_ProceduralOctaves = 4;
                terrain.m_ProceduralFrequency = 1.5f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                // Low relief: the plants, not the hillside, have to be what
                // changes between the two arms.
                terrain.m_HeightScale = 6.0f;
                terrain.m_TessellationEnabled = false;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (auto layer : TerrainGenerator::MakeDefaultLayers())
                {
                    layer.BaseColor = glm::vec3(0.4f); // neutral terrain cannot satisfy the green plant mask
                    terrain.m_Material->AddLayer(layer);
                }

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;

                FoliageLayer pines;
                pines.Name = "Pines";
                pines.MeshPath = kPineMesh;
                pines.AlbedoPath = kFoliageAlbedo;
                pines.Density = 0.02f;
                pines.SplatmapChannel = -1;
                pines.MinSlopeAngle = 0.0f;
                pines.MaxSlopeAngle = 60.0f;
                pines.MinScale = 1.0f;
                pines.MaxScale = 1.0f;
                // Sizeable plants: the mesh and the card have to be
                // distinguishable at the near camera's distance.
                pines.MinHeight = 8.0f;
                pines.MaxHeight = 12.0f;
                pines.ViewDistance = 400.0f;
                pines.FadeStartDistance = 360.0f;
                pines.UseAuthoredMesh = true;
                pines.MeshViewDistance = kMeshViewDistance;
                pines.MeshFadeStartDistance = kMeshFadeStart;
                pines.AlphaCutoff = 0.25f;
                pines.WindStrength = 2.0f;
                pines.WindStiffness = 0.4f;
                pines.WindBranchWeight = 0.7f;
                pines.WindLeafWeight = 0.8f;
                pines.BaseColor = glm::vec3(0.18f, 0.42f, 0.14f);
                foliage.m_Layers.Add(pines);
                foliage.m_NeedsRebuild = true;
            }
        }

        // Renders from `eye` and reads SceneColor, where the foliage pass
        // composites before post/UI.
        void Capture(const glm::vec3& eye, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, pitch);
            RunEditorFrames(camera, 4);
            {
                // TEMP diagnostic (#1484 foliage OOM): free device memory in KiB.
                ::glFinish();
                GLint nv = -1;
                GLint ati[4] = { -1, -1, -1, -1 };
                ::glGetIntegerv(0x9049, &nv); // GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
                ::glGetIntegerv(0x87FC, ati); // TEXTURE_FREE_MEMORY_ATI
                GLint atiVbo[4] = { -1, -1, -1, -1 };
                ::glGetIntegerv(0x87FB, atiVbo); // VBO_FREE_MEMORY_ATI
                while (::glGetError() != GL_NO_ERROR)
                {
                }
                long long vramUsed = -1;
                long long gttUsed = -1;
                long long visUsed = -1;
                for (int card = 0; card < 4 && vramUsed < 0; ++card)
                {
                    const std::string base = "/sys/class/drm/card" + std::to_string(card) + "/device/";
                    std::ifstream vram(base + "mem_info_vram_used");
                    std::ifstream gtt(base + "mem_info_gtt_used");
                    std::ifstream vis(base + "mem_info_vis_vram_used");
                    if (vis)
                        vis >> visUsed;
                    if (vram && gtt)
                    {
                        vram >> vramUsed;
                        gtt >> gttUsed;
                    }
                }
                std::printf("[foliage-mem] path %d msaa %u nvx %d ati-tex %d/%d ati-vbo %d/%d vram-used %lld MiB gtt-used "
                            "%lld MiB vis-used %lld MiB persistent %llu MiB in %llu allocs %llu\n",
                            static_cast<int>(Renderer3D::GetRendererSettings().Path),
                            Renderer3D::GetRendererSettings().Deferred.MSAASampleCount, nv, ati[0], ati[2], atiVbo[0],
                            atiVbo[2], vramUsed < 0 ? -1 : vramUsed >> 20, gttUsed < 0 ? -1 : gttUsed >> 20, visUsed < 0 ? -1 : visUsed >> 20,
                            s_PersistentBytes >> 20, s_PersistentCount, s_AllocationCalls);
                std::fflush(stdout);
            }

            {
                // TEMP census (#1484): every live GL texture and buffer.
                TextureCensus();
                unsigned long long total = 0, persistent = 0, count = 0;
                std::vector<std::pair<long long, GLuint>> sizes;
                for (GLuint id = 1; id < 60000; ++id)
                {
                    if (!::glIsBuffer(id))
                        continue;
                    GLint64 size = 0;
                    GLint flags = 0;
                    ::glGetNamedBufferParameteri64v(id, GL_BUFFER_SIZE, &size);
                    ::glGetNamedBufferParameteriv(id, GL_BUFFER_STORAGE_FLAGS, &flags);
                    total += static_cast<unsigned long long>(size);
                    ++count;
                    if ((flags & GL_MAP_PERSISTENT_BIT) != 0)
                        persistent += static_cast<unsigned long long>(size);
                    sizes.emplace_back(static_cast<long long>(size), id);
                }
                while (::glGetError() != GL_NO_ERROR)
                {
                }
                std::sort(sizes.begin(), sizes.end(), std::greater<>());
                std::printf("[foliage-buffers] %llu buffers, %llu MiB, persistent %llu MiB; largest:", count, total >> 20, persistent >> 20);
                for (size_t i = 0; i < sizes.size() && i < 6; ++i)
                    std::printf(" #%u=%lld MiB", sizes[i].second, sizes[i].first >> 20);
                std::printf("\n");
                std::fflush(stdout);
            }
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px)
        {
            ASSERT_EQ(px.size(), static_cast<sizet>(kWidth) * kHeight * 4);
            std::vector<u8> flipped(px);
            VisualEvidence::FlipRgbaRowsInPlace(flipped, kWidth, kHeight);
            fs::path dir = fs::path("assets") / "tests" / "visual";
            if (!Options().GoldenVendor.empty())
                dir /= Options().GoldenVendor;
            if (!GoldenRebaseRequested())
            {
                ::stbi_set_flip_vertically_on_load(0);
                ::stbi_set_flip_vertically_on_load_thread(0);
                int width = 0, height = 0, channels = 0;
                auto* data = ::stbi_load((dir / name).string().c_str(), &width, &height, &channels, 4);
                ASSERT_TRUE(data) << "Missing golden '" << (dir / name).generic_string()
                                  << "' (not an empty readback) -- rerun with --olo-golden-rebase to create it.";
                std::vector<u8> baseline;
                if (width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight))
                    baseline.assign(data, data + static_cast<sizet>(kWidth) * kHeight * 4);
                ::stbi_image_free(data);
                ASSERT_EQ(baseline.size(), flipped.size()) << name;
                const auto rmse = VisualEvidence::Rgba8Rmse(flipped, baseline) / 255.0;
                if (rmse < 0.002)
                    return;
                EXPECT_LE(rmse, 0.05) << name;
                EXPECT_GE(VisualEvidence::Rgba8Ssim(flipped, baseline, kWidth, kHeight), 0.985f) << name;
                return;
            }
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << ec.message();
            ASSERT_NE(::stbi_write_png((dir / name).string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       flipped.data(), static_cast<int>(kWidth) * 4),
                      0);
        }

        void SetWind(bool on)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            foliage.m_Layers[0].WindStrength = on ? 2.0f : 0.0f;
            foliage.m_NeedsRebuild = true;
        }

        std::vector<f32> Velocity()
        {
            std::vector<f32> values;
            const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::Velocity);
            EXPECT_NE(texture, 0u);
            if (texture != 0)
                ReadbackRgbaFloat(texture, kWidth, kHeight, values);
            return values;
        }

        std::vector<f32> Shadow()
        {
            const u32 texture = Renderer3D::ResolveFrameGraphTexture(ResourceNames::ShadowMapCSMCascade0);
            EXPECT_NE(texture, 0u);
            if (texture == 0)
                return {};
            GLint width = 0, height = 0;
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
            glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
            std::vector<f32> values(static_cast<sizet>(width) * static_cast<sizet>(height));
            glGetTextureSubImage(texture, 0, 0, 0, 0, width, height, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                                 static_cast<GLsizei>(values.size() * sizeof(f32)), values.data());
            return values;
        }

        static void WriteVelocity(const std::string& name, const std::vector<f32>& values)
        {
            std::vector<u8> pixels(values.size());
            for (sizet i = 0; i + 3 < values.size(); i += 4)
            {
                pixels[i] = static_cast<u8>(std::clamp(0.5f + 50.0f * values[i], 0.0f, 1.0f) * 255.0f);
                pixels[i + 1] = static_cast<u8>(std::clamp(0.5f + 50.0f * values[i + 1], 0.0f, 1.0f) * 255.0f);
                pixels[i + 2] = 0;
                pixels[i + 3] = 255;
            }
            WritePng(name, pixels);
        }

        Entity m_TerrainEntity;
    };

    TEST_F(FoliageWindEvidenceTest, EveryRasterPathShowsWindAndMotion)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        InstallAllocationLog();
        struct MockClock
        {
            ~MockClock()
            {
                Time::ClearMockTime();
            }
        } clock;
        f32 time = 4.0f;
        const glm::vec3 eye(128.0f, 12.0f, 150.0f);
        for (const auto path : { RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred })
        {
            const std::string pathName = path == RenderingPath::Forward ? "Forward" : path == RenderingPath::ForwardPlus ? "ForwardPlus"
                                                                                                                         : "Deferred";
            for (const u32 samples : { 1u, 4u })
            {
                // Only the deferred G-Buffer currently supports MSAA.
                if (samples > 1 && path != RenderingPath::Deferred)
                    continue;
                SCOPED_TRACE(pathName + " samples=" + std::to_string(samples));
                auto& settings = Renderer3D::GetRendererSettings();
                settings.Path = path;
                settings.Deferred.MSAASampleCount = samples;
                Renderer3D::ApplyRendererSettings();
                const std::string cell = "GL_" + pathName + "_MSAA" + std::to_string(samples);
                Time::SetMockTime(time);
                std::vector<u8> groundCapture;
                for (const bool oblique : { false, true })
                {
                    const glm::vec3 pose = oblique ? glm::vec3(160.0f, 22.0f, 160.0f) : eye;
                    const f32 yaw = oblique ? 0.5f : 0.0f;
                    const f32 pitch = oblique ? 0.3f : 0.06f;
                    std::vector<u8> on, repeat, off;
                    SetWind(true);
                    Capture(pose, yaw, pitch, on);
                    Capture(pose, yaw, pitch, repeat);
                    SetWind(false);
                    Capture(pose, yaw, pitch, off);
                    const f64 noise = VisualEvidence::Rgba8Rmse(on, repeat);
                    VisualEvidence::ExpectCapturesAreDistinct({ on, off }, { "wind", "off" }, noise);
                    const std::string angle = oblique ? "Oblique" : "Ground";
                    const auto isPlant = [](u32 r, u32 g, u32 b)
                    { return g > 12u && g > 1.4 * r && g > 1.4 * b; };
                    VisualEvidence::ExpectFrameHasSubject(on, angle, isPlant);
                    VisualEvidence::ExpectFrameHasSubject(off, angle + " off", isPlant);
                    if (oblique)
                        VisualEvidence::ExpectCapturesAreDistinct({ groundCapture, on }, { "ground", "oblique" }, noise);
                    else
                        groundCapture = on;
                    WritePng("FoliageWind_" + cell + "_" + angle + ".png", on);
                    WritePng("FoliageWindOff_" + cell + "_" + angle + ".png", off);
                }
                SetWind(true);
                std::vector<u8> warm;
                Capture(eye, 0.0f, 0.06f, warm);
                EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.5f, 2000.0f);
                camera.SetViewportSize(kWidth, kHeight);
                camera.SetPose(eye, 0.0f, 0.06f);
                time += 1.0f / 30.0f;
                Time::SetMockTime(time);
                RunEditorFrames(camera, 1);
                const auto moving = Velocity();
                GetScene().SetPaused(true);
                time += 1.0f / 30.0f;
                Time::SetMockTime(time);
                RunEditorFrames(camera, 1);
                const auto paused = Velocity();
                GetScene().SetPaused(false);
                ASSERT_EQ(moving.size(), paused.size());
                ASSERT_FALSE(moving.empty());
                sizet changed = 0;
                for (sizet i = 0; i + 3 < moving.size(); i += 4)
                {
                    EXPECT_TRUE(std::isfinite(moving[i]) && std::isfinite(moving[i + 1]));
                    EXPECT_TRUE(std::isfinite(paused[i]) && std::isfinite(paused[i + 1]));
                    EXPECT_LT(std::abs(paused[i]), 1e-4f);
                    EXPECT_LT(std::abs(paused[i + 1]), 1e-4f);
                    if (std::abs(moving[i] - paused[i]) + std::abs(moving[i + 1] - paused[i + 1]) > 1e-5f)
                        ++changed;
                }
                EXPECT_GT(changed, 20u) << "wind motion did not reach the velocity attachment";
                if (GoldenRebaseRequested())
                {
                    WriteVelocity("FoliageWind_" + cell + "_Velocity.png", moving);
                    WriteVelocity("FoliageWind_" + cell + "_Pause.png", paused);
                }
            }
        }
    }

    TEST_F(FoliageWindEvidenceTest, ForwardExportsRespectAmbientOcclusionDepthReaders)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const auto path : { RenderingPath::Forward, RenderingPath::ForwardPlus })
            for (const auto ao : { AOTechnique::SSAO, AOTechnique::GTAO })
            {
                auto& settings = Renderer3D::GetRendererSettings();
                settings.Path = path;
                auto& post = Renderer3D::GetPostProcessSettings();
                post.ActiveAOTechnique = ao;
                post.SSAOEnabled = ao == AOTechnique::SSAO;
                post.GTAOEnabled = ao == AOTechnique::GTAO;
                Renderer3D::ApplyRendererSettings();
                std::vector<u8> pixels;
                Capture({ 128.0f, 12.0f, 150.0f }, 0.0f, 0.06f, pixels);
                VisualEvidence::ExpectFrameHasSubject(pixels, "forward AO", [](u32 r, u32 g, u32 b)
                                                      { return g > 12u && g > 1.4 * r && g > 1.4 * b; });
            }
    }

    TEST_F(FoliageWindEvidenceTest, MissingAlbedoShadowDoesNotReuseThePreviousAtlas)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        auto& layer = foliage.m_Layers[0];
        layer.AlbedoPath.Empty();
        layer.UseAuthoredMesh = false;
        layer.UseImpostor = false;
        layer.WindStrength = 0.0f;
        foliage.m_NeedsRebuild = true;
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.5f, 2000.0f);
        camera.SetViewportSize(kWidth, kHeight);
        camera.SetPose({ 128.0f, 14.0f, 196.0f }, 0.0f, 6.0f);
        RunEditorFrames(camera, 4);
        ASSERT_TRUE(foliage.m_Renderer);
        const auto draws = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_FALSE(draws.IsEmpty());
        ASSERT_TRUE(std::ranges::all_of(std::span(draws.GetData(), static_cast<sizet>(draws.Num())), [](const auto& draw)
                                        { return draw.InstanceCount > 0 && !draw.AlbedoTextureID.IsValid(); }));

        GLStateGuard guard("MissingAlbedoShadow", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = spec.Height = 256;
        spec.Attachments = { FramebufferTextureFormat::DEPTH_COMPONENT32F };
        auto target = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/Foliage_Depth.glsl");
        ASSERT_TRUE(target && shader && shader->IsReady());
        auto cameraBuffer = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA);
        UBOStructures::CameraUBO light{};
        const glm::vec3 origin = Renderer3D::GetRenderOrigin();
        light.View = glm::lookAt(glm::vec3(128.0f, 80.0f, 300.0f) - origin,
                                 glm::vec3(128.0f, 8.0f, 128.0f) - origin, glm::vec3(0.0f, 1.0f, 0.0f));
        light.Projection = glm::ortho(-150.0f, 150.0f, -150.0f, 150.0f, 1.0f, 600.0f);
        light.ViewProjection = light.Projection * light.View;
        cameraBuffer->SetData(&light, UBOStructures::CameraUBO::GetSize());
        TextureSpecification textureSpec;
        textureSpec.GenerateMips = false;
        auto transparent = Texture2D::Create(textureSpec);
        std::array<u8, 4> transparentPixel{ 255, 255, 255, 0 };
        transparent->SetData(transparentPixel.data(), static_cast<u32>(transparentPixel.size()));
        const auto capture = [&](const Ref<Texture2D>& previouslyBound)
        {
            target->Bind();
            glViewport(0, 0, 256, 256);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glClearDepth(1.0);
            glClear(GL_DEPTH_BUFFER_BIT);
            cameraBuffer->Bind();
            HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE,
                                             previouslyBound->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
            // An UNCULLED shadow view index (past kMaxShadowViews): this fixture
            // renders the shadow depth of EVERY generated instance on purpose.
            // The GPU cull is exercised by FoliageGPUCullEvidenceTest; here an
            // unculled draw is the control the wind assertions were written
            // against (issue #1235).
            foliage.m_Renderer->RenderShadows(shader, 0.0f, FoliageGPUCuller::kMaxShadowViews);
            std::vector<f32> depths(256 * 256);
            glGetTextureImage(target->GetDepthAttachmentRendererID(), 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                              static_cast<GLsizei>(depths.size() * sizeof(f32)), depths.data());
            target->Unbind();
            return depths;
        };
        const auto afterTransparentAtlas = capture(transparent);
        const auto afterOpaqueTexture = capture(Renderer3D::GetWhiteTexture());
        sizet covered = 0;
        f32 maximumDifference = 0.0f;
        for (sizet i = 0; i < afterOpaqueTexture.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(afterOpaqueTexture[i]) && std::isfinite(afterTransparentAtlas[i]));
            covered += afterOpaqueTexture[i] < 0.99999f;
            maximumDifference = std::max(maximumDifference, std::abs(afterOpaqueTexture[i] - afterTransparentAtlas[i]));
        }
        EXPECT_GT(covered, 20u) << "positive control rendered no foliage shadow";
        EXPECT_LE(maximumDifference, 1e-6f) << "null-albedo shadow depends on the previous atlas alpha";
    }

    TEST_F(FoliageWindEvidenceTest, ShadowImpostorAndLodResetFollowTheDeformation)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct MockClock
        {
            ~MockClock()
            {
                Time::ClearMockTime();
            }
        } clock;
        f32 time = 4.0f;
        Time::SetMockTime(time);
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        settings.Deferred.MSAASampleCount = 1;
        Renderer3D::ApplyRendererSettings();
        const glm::vec3 eye(128.0f, 12.0f, 150.0f);
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.5f, 2000.0f);
        camera.SetViewportSize(kWidth, kHeight);
        camera.SetPose(eye, 0.0f, 0.06f);
        RunEditorFrames(camera, 4);
        const auto before = Shadow();
        ASSERT_FALSE(before.empty());
        // Several deterministic increments move the real shadow silhouette.
        for (u32 frame = 0; frame < 12; ++frame)
        {
            time += 1.0f / 30.0f;
            Time::SetMockTime(time);
            RunEditorFrames(camera, 1);
        }
        const auto after = Shadow();
        ASSERT_EQ(before.size(), after.size());
        sizet changed = 0;
        for (sizet i = 0; i < before.size(); ++i)
            if (std::abs(before[i] - after[i]) > 1e-6f)
                ++changed;
        EXPECT_GT(changed, 20u) << "wind changed colour but not the real CSM depth";

        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        foliage.m_Layers[0].UseImpostor = true;
        foliage.m_Layers[0].ImpostorFramesPerAxis = 4;
        foliage.m_Layers[0].ImpostorAtlasResolution = 256;
        foliage.m_NeedsRebuild = true;
        std::vector<u8> distantCapture;
        Capture({ 128.0f, 50.0f, 360.0f }, 0.0f, 0.22f, distantCapture);
        if (GoldenRebaseRequested())
            WritePng("FoliageWind_GL_Deferred_Impostor.png", distantCapture);
        auto info = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_TRUE(std::ranges::any_of(std::span(info.GetData(), static_cast<sizet>(info.Num())), [](const auto& draw)
                                        { return draw.UseImpostor; }));

        // LOD authoring changes reset wind once. Reprojecting the same new
        // representation must not invent motion from the discarded geometry.
        RunEditorFrames(camera, 4); // settle the near camera before testing representation history
        foliage.m_Layers[0].UseAuthoredMesh = false;
        foliage.m_NeedsRebuild = true;
        time += 1.0f / 30.0f;
        Time::SetMockTime(time);
        RunEditorFrames(camera, 1);
        const auto reset = Velocity();
        ASSERT_FALSE(reset.empty());
        f32 maximum = 0.0f;
        for (sizet i = 0; i + 3 < reset.size(); i += 4)
            maximum = std::max(maximum, std::abs(reset[i]) + std::abs(reset[i + 1]));
        EXPECT_LT(maximum, 1e-4f);
        if (GoldenRebaseRequested())
            WriteVelocity("FoliageWind_GL_Deferred_LodReset.png", reset);
    }
} // namespace OloEngine::Tests
