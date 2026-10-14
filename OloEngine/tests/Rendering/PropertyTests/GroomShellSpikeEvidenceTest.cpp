#include "OloEnginePCH.h"
// OLO_TEST_LAYER: L8
#include "../../Groom/GroomShellSpikeBake.h"
#include "../../Groom/GroomShellSpikeGLState.h"
#include "../../Groom/GroomLodFixture.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "RenderPropertyTest.h"
#include "OloEngine/Groom/GroomCoverage.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Shader.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image/stb_image_write.h>
#include <gtest/gtest.h>

#include <array>
#include <numeric>

namespace OloEngine::Tests::GroomShellSpike
{
    namespace
    {
        const std::filesystem::path Output = "../.v-build/1611";
        constexpr u32 Frames = 64;
        constexpr u32 Crop = 256;
        struct Vertex
        {
            glm::vec3 Position;
            glm::vec3 Tangent;
            glm::vec2 UV;
            f32 Alpha;
            u32 Id;
        };
        struct Parameters
        {
            glm::mat4 VP;
            glm::mat4 InverseVP;
            glm::vec4 ViewportFrame;
            glm::vec4 Layers;
            glm::vec4 Chart;
        };
        static_assert(sizeof(Parameters) == 176);
        static_assert(sizeof(Vertex) == 40);
        struct Mesh
        {
            u32 VAO = 0;
            u32 VBO = 0;
            u32 Count = 0;
            Mesh()
            {
                glCreateVertexArrays(1, &VAO);
                glCreateBuffers(1, &VBO);
                glVertexArrayVertexBuffer(VAO, 0, VBO, 0, sizeof(Vertex));
                for (u32 i = 0; i < 5; ++i)
                {
                    glEnableVertexArrayAttrib(VAO, i);
                    glVertexArrayAttribBinding(VAO, i, 0);
                }
                glVertexArrayAttribFormat(VAO, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, Position));
                glVertexArrayAttribFormat(VAO, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, Tangent));
                glVertexArrayAttribFormat(VAO, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, UV));
                glVertexArrayAttribFormat(VAO, 3, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, Alpha));
                glVertexArrayAttribIFormat(VAO, 4, 1, GL_UNSIGNED_INT, offsetof(Vertex, Id));
            }
            ~Mesh()
            {
                glDeleteVertexArrays(1, &VAO);
                glDeleteBuffers(1, &VBO);
            }
            Mesh(const Mesh&) = delete;
            Mesh& operator=(const Mesh&) = delete;
            void Upload(const std::vector<Vertex>& vertices)
            {
                Count = static_cast<u32>(vertices.size());
                glNamedBufferData(VBO, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STREAM_DRAW);
            }
        };
        struct Patch
        {
            Bake Data;
            Mesh Geometry;
            std::array<u32, 2> Textures{};
            f32 MaxLength = 0.0f;
            explicit Patch(Bake bake) : Data(std::move(bake))
            {
                for (const auto& map : Data.LengthDirection)
                {
                    MaxLength = std::max(MaxLength, map.x);
                }
                glCreateTextures(GL_TEXTURE_2D, 1, &Textures[0]);
                glTextureStorage2D(Textures[0], 1, GL_RGBA32F, MapWidth, MapHeight);
                glTextureSubImage2D(Textures[0], 0, 0, 0, MapWidth, MapHeight, GL_RGBA, GL_FLOAT, Data.LengthDirection.data());
                glCreateTextures(GL_TEXTURE_3D, 1, &Textures[1]);
                glTextureStorage3D(Textures[1], 6, GL_RG32F, MapWidth, MapHeight, HeightSlices);
                glTextureSubImage3D(Textures[1], 0, 0, 0, 0, MapWidth, MapHeight, HeightSlices, GL_RG, GL_FLOAT, Data.Density.data());
                glGenerateTextureMipmap(Textures[1]);
                for (u32 texture : Textures)
                {
                    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, texture == Textures[0] ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
                    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_REPEAT);
                    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTextureParameteri(texture, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
                }
                std::vector<Vertex> vertices;
                constexpr u32 sides = 64;
                constexpr u32 rings = 8;
                for (u32 y = 0; y < rings; ++y)
                {
                    for (u32 x = 0; x < sides; ++x)
                    {
                        const std::array<glm::vec2, 4> uv{
                            glm::vec2(static_cast<f32>(x) / sides, static_cast<f32>(y) / rings),
                            glm::vec2(static_cast<f32>(x + 1) / sides, static_cast<f32>(y) / rings),
                            glm::vec2(static_cast<f32>(x + 1) / sides, static_cast<f32>(y + 1) / rings),
                            glm::vec2(static_cast<f32>(x) / sides, static_cast<f32>(y + 1) / rings)
                        };
                        for (u32 i : { 0u, 1u, 2u, 0u, 2u, 3u })
                        {
                            vertices.push_back({ Surface(Data.Patch, uv[i]), Normal(uv[i].x), uv[i], 1.0f, 0 });
                        }
                    }
                }
                Geometry.Upload(vertices);
            }
            ~Patch()
            {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_3D, 0);
                glDeleteTextures(2, Textures.data());
            }
        };
        struct Target
        {
            u32 FBO = 0;
            std::array<u32, 2> Textures{};
            u32 ResolveFBO = 0;
            u32 ResolveTexture = 0;
            u32 Width;
            u32 Height;
            u32 Samples;
            Target(u32 width, u32 height, u32 samples) : Width(width), Height(height), Samples(samples)
            {
                glCreateFramebuffers(1, &FBO);
                const u32 type = samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
                glCreateTextures(type, 2, Textures.data());
                for (u32 i = 0; i < 2; ++i)
                {
                    const u32 format = i == 0 ? GL_RGBA32F : GL_DEPTH_COMPONENT32F;
                    if (samples > 1)
                    {
                        glTextureStorage2DMultisample(Textures[i], static_cast<GLsizei>(samples), format, width, height, GL_TRUE);
                    }
                    else
                    {
                        glTextureStorage2D(Textures[i], 1, format, width, height);
                    }
                    glNamedFramebufferTexture(FBO, i == 0 ? GL_COLOR_ATTACHMENT0 : GL_DEPTH_ATTACHMENT, Textures[i], 0);
                }
                glCreateFramebuffers(1, &ResolveFBO);
                glCreateTextures(GL_TEXTURE_2D, 1, &ResolveTexture);
                glTextureStorage2D(ResolveTexture, 1, GL_RGBA32F, width, height);
                glNamedFramebufferTexture(ResolveFBO, GL_COLOR_ATTACHMENT0, ResolveTexture, 0);
                if (glCheckNamedFramebufferStatus(FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
                    glCheckNamedFramebufferStatus(ResolveFBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                {
                    throw std::runtime_error("shell spike framebuffer is incomplete");
                }
            }
            ~Target()
            {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &FBO);
                glDeleteFramebuffers(1, &ResolveFBO);
                glDeleteTextures(2, Textures.data());
                glDeleteTextures(1, &ResolveTexture);
            }
            std::vector<f32> Read() const
            {
                glBlitNamedFramebuffer(FBO, ResolveFBO, 0, 0, Width, Height, 0, 0, Width, Height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                std::vector<f32> pixels(Crop * Crop * 4);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, ResolveFBO);
                glReadPixels(static_cast<GLint>(Width / 2 - Crop / 2), static_cast<GLint>(Height / 2 - Crop / 2), Crop, Crop, GL_RGBA, GL_FLOAT, pixels.data());
                return pixels;
            }
        };
        struct Camera
        {
            glm::mat4 View;
            glm::mat4 Projection;
        };
        Camera MakeCamera(const GroomAsset& groom, const Target& target, f32 pixels, u32 angle, f32 motion)
        {
            const glm::vec3 centre = (groom.GetBoundsMin() + groom.GetBoundsMax()) * 0.5f;
            const f32 radius = glm::length(groom.GetBoundsMax() - groom.GetBoundsMin()) * 0.5f;
            const f32 fov = glm::radians(45.0f);
            const f32 distance = radius / std::tan(fov * 0.5f) * target.Height / pixels;
            const std::array<glm::vec3, 3> directions{
                glm::vec3(0.0f, 0.25f, 1.0f), glm::vec3(1.0f, 0.4f, 1.0f), glm::vec3(1.0f, 0.1f, 0.05f)
            };
            const glm::vec3 eye = centre + glm::normalize(directions[angle] + glm::vec3(motion, 0.0f, 0.0f)) * distance;
            return { glm::lookAt(eye, centre, glm::vec3(0.0f, 1.0f, 0.0f)),
                     glm::perspective(fov, static_cast<f32>(target.Width) / target.Height, std::max(0.001f, distance * 0.01f), distance * 4.0f) };
        }
        void UploadRibbons(Mesh& mesh, const GroomAsset& groom, const Camera& camera, const Target& target,
                           f32 pixelSize, const GroomLodLevel* level, u32 baseCount)
        {
            std::vector<GroomCoverage::ScreenSegment> segments;
            const auto stats = GroomCoverage::ProjectGroom(groom, glm::mat4(1.0f), camera.View, camera.Projection, target.Width, target.Height, 1.0f, segments);
            if (stats.SegmentsProjected == 0)
            {
                throw std::runtime_error("empty reference projection");
            }
            std::vector<Vertex> vertices;
            vertices.reserve(segments.size() * 6);
            sizet segment = 0;
            for (u32 c = 0; c < groom.GetCurveCount(); ++c)
            {
                const u32 first = groom.GetCurveFirstPoint(c);
                for (u32 p = 1; p < groom.GetCurvePointCount(c); ++p)
                {
                    auto s = segments.at(segment++);
                    if (level)
                    {
                        const f32 reach = GroomCardCoverageGrowthReach(baseCount, groom.GetCurveCount());
                        const f32 footprint = std::max(1.0e-3f, 1.0f - GroomCardCoverageGrowthBlend(pixelSize, level->SourcePixelSize));
                        const f32 growthA = GroomCardReachedGrowth(level->PointCoverageGrowth[first + p - 1], reach);
                        const f32 growthB = GroomCardReachedGrowth(level->PointCoverageGrowth[first + p], reach);
                        s.HalfWidthA *= GroomCardCoverageWiden(2.0f * s.HalfWidthA, 2.0f * s.HalfWidthA * growthA, footprint);
                        s.HalfWidthB *= GroomCardCoverageWiden(2.0f * s.HalfWidthB, 2.0f * s.HalfWidthB * growthB, footprint);
                    }
                    const glm::vec2 delta = s.B - s.A;
                    const f32 length = glm::length(delta);
                    if (length <= 1.0e-6f)
                    {
                        continue;
                    }
                    const glm::vec2 normal{ -delta.y / length, delta.x / length };
                    const glm::vec3 tangent = glm::normalize(groom.GetPoints()[first + p] - groom.GetPoints()[first + p - 1]);
                    const std::array<glm::vec2, 4> points{ s.A - normal * std::max(s.HalfWidthA, 0.5f), s.A + normal * std::max(s.HalfWidthA, 0.5f),
                                                           s.B + normal * std::max(s.HalfWidthB, 0.5f), s.B - normal * std::max(s.HalfWidthB, 0.5f) };
                    for (u32 i : { 0u, 1u, 2u, 0u, 2u, 3u })
                    {
                        const f32 alpha = std::min(1.0f, (i < 2 ? s.HalfWidthA : s.HalfWidthB) * 2.0f);
                        vertices.push_back({ { points[i].x / target.Width * 2.0f - 1.0f, 1.0f - points[i].y / target.Height * 2.0f,
                                               (i < 2 ? s.DepthA : s.DepthB) * 2.0f - 1.0f },
                                             tangent,
                                             {},
                                             alpha,
                                             s.Id });
                    }
                }
            }
            mesh.Upload(vertices);
        }
        void SaveImage(const std::string& name, const std::vector<f32>& pixels, const Target& target)
        {
            // The 256-square readback contains the complete projected bounds.
            // Render/query dimensions remain 720p/1080p; only transfer is cropped.
            (void)target;
            std::vector<u8> rgba(Crop * Crop * 4);
            for (u32 y = 0; y < Crop; ++y)
            {
                for (u32 x = 0; x < Crop; ++x)
                {
                    const sizet source = ((Crop - 1 - y) * Crop + x) * 4;
                    const sizet dest = (y * Crop + x) * 4;
                    for (u32 channel = 0; channel < 3; ++channel)
                    {
                        rgba[dest + channel] = static_cast<u8>(std::clamp(pixels[source + channel] * 255.0f, 0.0f, 255.0f));
                    }
                    rgba[dest + 3] = 255;
                }
            }
            if (stbi_write_png((Output / (name + ".png")).string().c_str(), Crop, Crop, 4, rgba.data(), Crop * 4) == 0)
            {
                throw std::runtime_error("shell spike PNG write failed");
            }
        }
        struct Result
        {
            std::vector<f32> Mean;
            f64 Area = 0.0;
            f64 Energy = 0.0;
            f64 GpuMs = 0.0;
            f64 Temporal = 0.0;
            f64 AreaStandardError = 0.0;
            f64 EnergyStandardError = 0.0;
            std::vector<std::vector<f32>> FrameCoverage;
            f64 NativeError = 0.0;
            f64 MotionResidual = 0.0;
        };
        struct Probe
        {
            Ref<Shader> RibbonShader = Shader::Create("assets/shaders/tests/GroomShellSpikeRibbon.glsl");
            Ref<Shader> ShellShader = Shader::Create("assets/shaders/tests/GroomShellSpikeShell.glsl");
            Mesh Ribbons;
            std::vector<std::unique_ptr<Patch>> Patches;
            u32 UBO = 0;
            const GroomAsset* RibbonSource = nullptr;
            const GroomLodLevel* CardLevel = nullptr;
            u32 BaseCount = 0;
            const Result* FrameReference = nullptr;
            explicit Probe(const GroomAsset& groom, bool longCoat, bool bodyOnly = false, const std::string& artifactPrefix = {})
            {
                BaseCount = groom.GetCurveCount();
                glCreateBuffers(1, &UBO);
                glNamedBufferData(UBO, sizeof(Parameters), nullptr, GL_DYNAMIC_DRAW);
                const auto charts = Charts(longCoat);
                for (const auto& chart : charts)
                {
                    if (bodyOnly && chart.Name != "body")
                    {
                        continue;
                    }
                    auto bake = BakeChart(groom, chart, charts);
                    std::ofstream mapFile(Output / (artifactPrefix + (bodyOnly ? "control_" : longCoat ? "long_"
                                                                                                       : "short_") +
                                                    chart.Name + "_maps.bin"),
                                          std::ios::binary);
                    mapFile.exceptions(std::ios::failbit | std::ios::badbit);
                    mapFile.write(reinterpret_cast<const char*>(bake.LengthDirection.data()), static_cast<std::streamsize>(bake.LengthDirection.size() * sizeof(glm::vec4)));
                    mapFile.write(reinterpret_cast<const char*>(bake.Density.data()), static_cast<std::streamsize>(bake.Density.size() * sizeof(glm::vec2)));
                    mapFile.close();
                    Patches.push_back(std::make_unique<Patch>(std::move(bake)));
                }
            }
            ~Probe()
            {
                glBindBufferBase(GL_UNIFORM_BUFFER, 30, 0);
                glUseProgram(0);
                glBindVertexArray(0);
                glDeleteBuffers(1, &UBO);
            }
            void Clear(const Target& target)
            {
                const std::array<f32, 4> clear{};
                const f32 depth = 1.0f;
                glClearNamedFramebufferfv(target.FBO, GL_COLOR, 0, clear.data());
                glClearNamedFramebufferfv(target.FBO, GL_DEPTH, 0, &depth);
            }
            void Draw(const Target& target, const Camera& camera, u32 layers, u32 frame, bool jitter)
            {
                glBindFramebuffer(GL_FRAMEBUFFER, target.FBO);
                glViewport(0, 0, target.Width, target.Height);
                glBindBufferBase(GL_UNIFORM_BUFFER, 30, UBO);
                const glm::mat4 vp = camera.Projection * camera.View;
                Parameters parameters{ vp, glm::inverse(vp), { target.Width, target.Height, frame, jitter ? 1.0f : 0.0f }, { layers, 0.0f, 0.0f, 0.0f }, {} };
                if (layers == 0)
                {
                    RibbonShader->Bind();
                    glNamedBufferSubData(UBO, 0, sizeof(parameters), &parameters);
                    glBindVertexArray(Ribbons.VAO);
                    glDrawArrays(GL_TRIANGLES, 0, Ribbons.Count);
                }
                else
                {
                    ShellShader->Bind();
                    u32 chartId = 0;
                    for (const auto& patch : Patches)
                    {
                        parameters.Layers.y = TwoPi * patch->Data.Patch.Radius;
                        parameters.Layers.z = patch->Data.Patch.To - patch->Data.Patch.From;
                        parameters.Layers.w = static_cast<f32>(chartId++);
                        parameters.Chart = { patch->Data.Patch.Radius, patch->Data.Patch.From, patch->Data.Patch.To, patch->MaxLength };
                        glNamedBufferSubData(UBO, 0, sizeof(parameters), &parameters);
                        glBindTextureUnit(0, patch->Textures[0]);
                        glBindTextureUnit(1, patch->Textures[1]);
                        glBindVertexArray(patch->Geometry.VAO);
                        glDrawArraysInstanced(GL_TRIANGLES, 0, patch->Geometry.Count, static_cast<GLsizei>(layers));
                    }
                }
            }
            Result Measure(const GroomAsset& groom, const Target& target, f32 size, u32 angle, u32 layers, bool jitter, bool motion, const std::string& name, std::ofstream& timings)
            {
                Result result;
                result.Mean.resize(Crop * Crop * 4, 0.0f);
                const auto camera = MakeCamera(groom, target, size, angle, 0.0f);
                // Reference/card projection and uploads are deliberately outside
                // GPU queries. Timings describe raster pass cost, not CPU setup.
                if (layers == 0)
                {
                    UploadRibbons(Ribbons, RibbonSource ? *RibbonSource : groom, camera, target, size, CardLevel, BaseCount);
                }
                for (u32 frame = 0; frame < Frames; ++frame)
                {
                    Clear(target);
                    Draw(target, camera, layers, frame, jitter);
                }
                std::array<u32, Frames> queries{};
                glGenQueries(Frames, queries.data());
                for (u32 frame = 0; frame < Frames; ++frame)
                {
                    const f32 offset = motion ? (static_cast<f32>(frame) / (Frames - 1) - 0.5f) * 0.12f : 0.0f;
                    const auto current = MakeCamera(groom, target, size, angle, offset);
                    if (layers == 0 && motion)
                    {
                        UploadRibbons(Ribbons, RibbonSource ? *RibbonSource : groom, current, target, size, CardLevel, BaseCount);
                    }
                    Clear(target);
                    glBeginQuery(GL_TIME_ELAPSED, queries[frame]);
                    Draw(target, current, layers, frame, jitter);
                    glEndQuery(GL_TIME_ELAPSED);
                }
                std::vector<f64> samples;
                for (u32 frame = 0; frame < Frames; ++frame)
                {
                    GLuint64 elapsed = 0;
                    glGetQueryObjectui64v(queries[frame], GL_QUERY_RESULT, &elapsed);
                    samples.push_back(static_cast<f64>(elapsed) / 1.0e6);
                    timings << name << ',' << frame << ',' << samples.back() << '\n';
                }
                glDeleteQueries(Frames, queries.data());
                std::sort(samples.begin(), samples.end());
                result.GpuMs = (samples[31] + samples[32]) * 0.5;
                std::vector<f32> previous;
                std::ofstream sequence(Output / (name + "_sequence.csv"));
                sequence << "frame,area,energy,mean_area,mean_energy,delta_area\n";
                f64 areaSquares = 0.0;
                f64 energySquares = 0.0;
                for (u32 frame = 0; frame < Frames; ++frame)
                {
                    const f32 offset = motion ? (static_cast<f32>(frame) / (Frames - 1) - 0.5f) * 0.12f : 0.0f;
                    const auto current = MakeCamera(groom, target, size, angle, offset);
                    if (layers == 0 && motion)
                    {
                        UploadRibbons(Ribbons, RibbonSource ? *RibbonSource : groom, current, target, size, CardLevel, BaseCount);
                    }
                    Clear(target);
                    Draw(target, current, layers, frame, jitter);
                    auto pixels = target.Read();
                    std::vector<f32> coverage(Crop * Crop);
                    f64 frameArea = 0.0;
                    f64 frameEnergy = 0.0;
                    f64 frameDelta = 0.0;
                    f64 meanArea = 0.0;
                    f64 meanEnergy = 0.0;
                    for (sizet i = 0; i < pixels.size(); ++i)
                    {
                        if (!std::isfinite(pixels[i]))
                        {
                            throw std::runtime_error("non-finite shell spike output");
                        }
                        result.Mean[i] += pixels[i] / Frames;
                        if (i % 4 == 0)
                        {
                            const sizet pixel = i / 4;
                            coverage[pixel] = pixels[i];
                            frameArea += pixels[i];
                            meanArea += result.Mean[i] * Frames / (frame + 1);
                            if (FrameReference)
                            {
                                result.NativeError += std::abs(pixels[i] - FrameReference->FrameCoverage[frame][pixel]) / (Frames * Crop * Crop);
                                if (frame > 0)
                                {
                                    const f32 referenceDelta = FrameReference->FrameCoverage[frame][pixel] - FrameReference->FrameCoverage[frame - 1][pixel];
                                    result.MotionResidual += std::abs(pixels[i] - previous[i] - referenceDelta) / ((Frames - 1) * Crop * Crop);
                                }
                            }
                        }
                        if (i % 4 == 1)
                        {
                            frameEnergy += pixels[i];
                            meanEnergy += result.Mean[i] * Frames / (frame + 1);
                        }
                        if (i % 4 == 0 && !previous.empty())
                        {
                            result.Temporal += std::abs(pixels[i] - previous[i]) / (Frames - 1);
                            frameDelta += std::abs(pixels[i] - previous[i]);
                        }
                    }
                    sequence << frame << ',' << frameArea << ',' << frameEnergy << ',' << meanArea << ',' << meanEnergy << ',' << frameDelta << '\n';
                    areaSquares += frameArea * frameArea;
                    energySquares += frameEnergy * frameEnergy;
                    if (frame == 0 || (motion && frame % 8 == 0))
                    {
                        SaveImage(name + "_frame" + std::to_string(frame), pixels, target);
                    }
                    previous = std::move(pixels);
                    result.FrameCoverage.push_back(std::move(coverage));
                }
                for (sizet i = 0; i < result.Mean.size(); i += 4)
                {
                    result.Area += result.Mean[i];
                    result.Energy += result.Mean[i + 1];
                }
                SaveImage(name + "_mean64", result.Mean, target);
                result.AreaStandardError = std::sqrt(std::max(0.0, areaSquares / Frames - result.Area * result.Area) / (Frames - 1));
                result.EnergyStandardError = std::sqrt(std::max(0.0, energySquares / Frames - result.Energy * result.Energy) / (Frames - 1));
                return result;
            }
        };
        void Run(bool longCoat)
        {
            ExtraState extraState;
            GLStateGuard guard("GroomShellSpike", GLStateGuard::Policy::Restore);
            std::filesystem::create_directories(Output);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glFrontFace(GL_CCW);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            glDisable(GL_SAMPLE_MASK);
            glEnable(GL_MULTISAMPLE);
            glDisable(GL_STENCIL_TEST);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            const auto groom = Load(longCoat);
            ASSERT_FALSE(groom->GetLodLevels().empty());
            std::string reason;
            const auto cards = GroomLodFixture::MakeGroomFromLevel(*groom, groom->GetLodLevels()[0], reason);
            ASSERT_TRUE(cards) << reason;
            const std::string coat = longCoat ? "long" : "short";
            std::ofstream provenance(Output / (coat + "_provenance.txt"));
            provenance << glGetString(GL_VENDOR) << '\n'
                       << glGetString(GL_RENDERER) << '\n'
                       << glGetString(GL_VERSION) << '\n'
                       << "strands=" << groom->GetCurveCount() << " cards=" << cards->GetCurveCount() << " frames=" << Frames << '\n';
            provenance << "cardSourcePixelSize=" << groom->GetLodLevels()[0].SourcePixelSize << " growthReach=" << GroomCardCoverageGrowthReach(groom->GetCurveCount(), cards->GetCurveCount()) << '\n';
            std::ofstream metrics(Output / (coat + "_metrics.csv"));
            metrics << "name,pixels,angle,width,height,samples,layers,jitter,motion,area,energy,gpu_ms,temporal,coverage_ratio,energy_ratio,mean_abs,card_mean_abs,area_se,energy_se,native_error,motion_residual\n";
            std::ofstream timings(Output / (coat + "_timings.csv"));
            timings << "name,frame,gpu_ms\n";
            Probe probe(*groom, longCoat);
            for (u32 config = 0; config < 3; ++config)
            {
                Target target(config == 2 ? 1920 : 1280, config == 2 ? 1080 : 720, config == 1 ? 4 : 1);
                GLint actualSamples = 0;
                glGetTextureLevelParameteriv(target.Textures[0], 0, GL_TEXTURE_SAMPLES, &actualSamples);
                provenance << "config=" << config << " actualSamples=" << actualSamples << '\n';
                for (f32 size : { 128.0f, 64.0f, 32.0f, 16.0f, 8.0f, 4.0f })
                {
                    for (u32 angle = 0; angle < 3; ++angle)
                    {
                        // All configurations have the full size/angle matrix;
                        // motion is a separate matched 32-pixel sequence.
                        const u32 cases = config == 0 && std::abs(size - 32.0f) < 0.1f ? 2 : 1;
                        for (u32 sequence = 0; sequence < cases; ++sequence)
                        {
                            const bool motion = sequence > 0;
                            const std::string prefix = coat + "_c" + std::to_string(config) + "_p" + std::to_string(static_cast<u32>(size)) + "_a" + std::to_string(angle) + (motion ? "_motion" : "_static");
                            // Cards share the strand camera, bounds and history.
                            // MakeCamera uses the base groom for every arm.
                            auto reference = probe.Measure(*groom, target, size, angle, 0, false, motion, prefix + "_strands", timings);
                            ASSERT_GT(reference.Area, 0.0) << prefix;
                            if (!motion && config == 0)
                            {
                                // #1252's independent true-width, square-ended
                                // 8x8 raster records an independent area bridge.
                                const auto camera = MakeCamera(*groom, target, size, angle, 0.0f);
                                std::vector<GroomCoverage::ScreenSegment> segments;
                                const auto projected = GroomCoverage::ProjectGroom(*groom, glm::mat4(1.0f), camera.View, camera.Projection, target.Width, target.Height, 1.0f, segments);
                                ASSERT_GT(projected.SegmentsProjected, 0u);
                                const auto analytic = GroomCoverage::ReferenceCoverage(segments, target.Width, target.Height, 8);
                                const f64 analyticArea = std::accumulate(analytic.begin(), analytic.end(), 0.0);
                                provenance << prefix << " analytic_area=" << analyticArea << " stochastic_area=" << reference.Area << " se=" << reference.AreaStandardError << '\n';
                                provenance.flush();
                            }
                            probe.FrameReference = &reference;
                            probe.RibbonSource = cards.Raw();
                            probe.CardLevel = &groom->GetLodLevels()[0];
                            auto cardResult = probe.Measure(*groom, target, size, angle, 0, false, motion, prefix + "_cards", timings);
                            probe.RibbonSource = nullptr;
                            probe.CardLevel = nullptr;
                            f64 cardError = 0.0;
                            for (sizet i = 0; i < reference.Mean.size(); i += 4)
                            {
                                cardError += std::abs(reference.Mean[i] - cardResult.Mean[i]);
                            }
                            const f64 pixelCount = Crop * Crop;
                            auto write = [&](const std::string& arm, const Result& result, u32 layers, bool jitter)
                            {
                                f64 error = 0.0;
                                for (sizet i = 0; i < reference.Mean.size(); i += 4)
                                {
                                    error += std::abs(reference.Mean[i] - result.Mean[i]);
                                }
                                metrics << prefix + "_" + arm << ',' << size << ',' << angle << ',' << target.Width << ',' << target.Height << ',' << target.Samples << ','
                                        << layers << ',' << jitter << ',' << motion << ',' << result.Area << ',' << result.Energy << ',' << result.GpuMs << ',' << result.Temporal << ','
                                        << result.Area / reference.Area << ',' << result.Energy / reference.Energy << ',' << error / pixelCount << ',' << cardError / pixelCount << ',' << result.AreaStandardError << ',' << result.EnergyStandardError << ',' << result.NativeError << ',' << result.MotionResidual << '\n';
                                metrics.flush();
                            };
                            write("strands", reference, 0, false);
                            write("cards", cardResult, 0, false);
                            for (u32 layers : { 8u, 12u, 16u })
                            {
                                for (bool jitter : { false, true })
                                {
                                    const std::string arm = "shell" + std::to_string(layers) + (jitter ? "_jitter" : "_fixed");
                                    const auto result = probe.Measure(*groom, target, size, angle, layers, jitter, motion, prefix + "_" + arm, timings);
                                    write(arm, result, layers, jitter);
                                }
                            }
                            probe.FrameReference = nullptr;
                            ASSERT_EQ(glGetError(), GL_NO_ERROR) << prefix;
                        }
                    }
                }
            }
        }
        void RunControls(const std::string& artifactPrefix = {})
        {
            ExtraState extra;
            GLStateGuard guard("GroomShellSpikeControls", GLStateGuard::Policy::Restore);
            std::filesystem::create_directories(Output);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            glDisable(GL_SAMPLE_MASK);
            glFrontFace(GL_CCW);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LESS);
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            const auto charts = Charts(false);
            const auto& chart = charts[0];
            const glm::vec2 uv{ 0.1273f, 0.513f };
            const auto root = Surface(chart, uv);
            const auto normal = Normal(uv.x);
            const glm::vec3 circumferential{ -normal.y, normal.x, 0.0f };
            Target target(1280, 720, 1);
            std::ofstream timings(Output / (artifactPrefix + "controls_timings.csv"));
            timings << "name,frame,gpu_ms\n";
            std::ofstream metrics(Output / (artifactPrefix + "controls_metrics.csv"));
            metrics << "kind,angle,layers,area,strand_area,ratio,gpu_ms\n";
            for (u32 kind = 0; kind < 3; ++kind)
            {
                GroomBuilder builder;
                u16 group = 0;
                std::string reason;
                ASSERT_TRUE(builder.AddGroup("/body#0", group, reason));
                std::array<glm::vec3, 5> points{};
                std::array<f32, 5> widths{};
                for (u32 p = 0; p < points.size(); ++p)
                {
                    const f32 t = static_cast<f32>(p) / (points.size() - 1);
                    points[p] = root + normal * t * 0.012f;
                    if (kind > 0)
                    {
                        points[p].z += t * 0.006f;
                    }
                    if (kind == 2)
                    {
                        points[p] += circumferential * (t * t * 0.003f);
                    }
                    widths[p] = 0.0002f;
                }
                GroomCurveInput curve;
                curve.Points = points;
                curve.Widths = widths;
                curve.RootUV = { uv.x, chart.VFrom + uv.y * (chart.VTo - chart.VFrom) };
                curve.GroupId = group;
                ASSERT_TRUE(builder.AddCurve(curve, reason));
                const auto groom = builder.Build(reason);
                ASSERT_TRUE(groom) << reason;
                Probe probe(*groom, false, true, artifactPrefix);
                for (u32 angle = 0; angle < 3; ++angle)
                {
                    const std::string name = artifactPrefix + "control" + std::to_string(kind) + "_a" + std::to_string(angle);
                    // Use a wider framing for every isolated-fibre view so
                    // the camera stays outside the full parent chart. Proxy
                    // geometry and the strand reference share that camera.
                    const auto reference = probe.Measure(*groom, target, 16.0f, angle, 0, false, false, name + "_strands", timings);
                    ASSERT_GT(reference.Area, 0.0);
                    f64 firstArea = 0.0;
                    for (u32 layers : { 8u, 12u, 16u })
                    {
                        const auto result = probe.Measure(*groom, target, 16.0f, angle, layers, true, false, name + "_shell" + std::to_string(layers), timings);
                        metrics << kind << ',' << angle << ',' << layers << ',' << result.Area << ',' << reference.Area << ',' << result.Area / reference.Area << ',' << result.GpuMs << '\n';
                        std::printf("[shell-control] kind=%u angle=%u layers=%u area=%.6f reference=%.6f ratio=%.6f\n", kind, angle, layers, result.Area, reference.Area, result.Area / reference.Area);
                        if (layers == 8)
                        {
                            firstArea = result.Area;
                        }
                        // An oblique-plane control isolates the old 1/N defect.
                        // Grazing views measure the candidate's silhouette limit.
                        if (angle == 2 && kind > 0 && layers == 16)
                        {
                            EXPECT_GT(result.Area, firstArea * 0.7);
                            EXPECT_LT(result.Area, firstArea * 1.3);
                        }
                        if (angle == 2)
                        {
                            // An independent projected strand arm catches a
                            // shader-wide opacity scale error. Allow measured
                            // finite-history uncertainty plus 10% voxel error;
                            // this is an instrument check, not the coat's bar.
                            const f64 uncertainty = 4.0 * std::hypot(result.AreaStandardError, reference.AreaStandardError);
                            EXPECT_NEAR(result.Area, reference.Area, uncertainty + reference.Area * 0.10);
                        }
                    }
                }
                metrics.flush();
                ASSERT_EQ(glGetError(), GL_NO_ERROR);
            }
        }
        void CheckStateRestoration()
        {
            ExtraState original;
            GLStateGuard guard("GroomShellSpikeStateContract", GLStateGuard::Policy::Restore);
            u32 buffer = 0;
            glCreateBuffers(1, &buffer);
            GLint alignment = 0;
            glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
            glNamedBufferData(buffer, alignment * 3, nullptr, GL_STATIC_DRAW);
            glBindBufferRange(GL_UNIFORM_BUFFER, 30, buffer, alignment, alignment);
            glEnablei(GL_BLEND, 1);
            glBlendFuncSeparatei(1, GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
            glBlendEquationSeparatei(1, GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
            glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
            RunControls("state_");
            GLint binding = 0;
            GLint64 start = 0;
            GLint64 size = 0;
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 30, &binding);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_START, 30, &start);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, 30, &size);
            EXPECT_EQ(static_cast<u32>(binding), buffer);
            EXPECT_EQ(start, alignment);
            EXPECT_EQ(size, alignment);
            EXPECT_TRUE(glIsEnabledi(GL_BLEND, 1));
            GLint source = 0;
            GLint equation = 0;
            glGetIntegeri_v(GL_BLEND_SRC_RGB, 1, &source);
            glGetIntegeri_v(GL_BLEND_EQUATION_RGB, 1, &equation);
            EXPECT_EQ(source, GL_ZERO);
            EXPECT_EQ(equation, GL_FUNC_REVERSE_SUBTRACT);
            std::array<GLboolean, 4> mask{};
            glGetBooleani_v(GL_COLOR_WRITEMASK, 1, mask.data());
            const std::array<GLboolean, 4> expected{ GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE };
            EXPECT_EQ(mask, expected);
            glBindBufferBase(GL_UNIFORM_BUFFER, 30, 0);
            glDeleteBuffers(1, &buffer);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }
    } // namespace
} // namespace OloEngine::Tests::GroomShellSpike

TEST(GroomShellSpikeShortEvidence, CookedCoatComparison)
{
    OLO_ENSURE_GPU_OR_SKIP();
    OloEngine::Tests::GroomShellSpike::Run(false);
}
TEST(GroomShellSpikeLongEvidence, CookedCoatComparison)
{
    OLO_ENSURE_GPU_OR_SKIP();
    OloEngine::Tests::GroomShellSpike::Run(true);
}
TEST(GroomShellSpikeGpuContract, IsolatedRadialTiltedAndBentFibres)
{
    OLO_ENSURE_GPU_OR_SKIP();
    OloEngine::Tests::GroomShellSpike::RunControls();
}
TEST(GroomShellSpikeGpuContract, IndexedStateAndRangedBindingsSurviveTheProbe)
{
    OLO_ENSURE_GPU_OR_SKIP();
    OloEngine::Tests::GroomShellSpike::CheckStateRestoration();
}
