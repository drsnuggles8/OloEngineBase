// OLO_TEST_LAYER: L8
// =============================================================================
// GaussianSplatVisualEvidenceTest.cpp — the rendered half of the Gaussian-splat
// viability spike (issue #971), and the place its numbers are measured.
//
// Two jobs, one harness, because they need the same buffers and the same GL
// context:
//
//   1. EVIDENCE. Render the checked-in fixture cloud from four camera poses
//      through SplatSpike_Gaussian.glsl and write each frame to
//      OloEditor/assets/tests/visual/Splat_<pose>.png, so the quality claim in
//      the decision record has pictures behind it. The assertions check the
//      things a human eye would check anyway (the three discs are present in
//      their own colours; the semi-transparent shell reads through them), plus
//      one a human eye cannot: that rendering in the WRONG order produces a
//      measurably different image, which is what makes the ordering claim
//      falsifiable rather than decorative.
//
//   2. MEASUREMENT. Time the CPU ordering pass and the GPU splat pass at
//      4k / 100k / 500k splats, against an opaque control drawing the same
//      quads at the same screen positions. The numbers are PRINTED, not
//      asserted, because a perf assertion on a dev workstation is a flake
//      (oloengine-perf-tests-are-dev-workstation-only); the decision record
//      quotes the printed run.
//
// SKIPs cleanly with no GL 4.6 context, so headless CI is a no-op.
// Run from OloEditor/ so the shaders and the fixture resolve.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"
#include "OloEngine/Renderer/Splat/GaussianSplatView.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "PropertyTests/RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <numeric>
#include <random>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;
    using namespace OloEngine::GaussianSplat;

    namespace
    {
        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // The house clear colour for evidence captures (visual-evidence tests
        // clear to mid-grey, not black), so these PNGs sit next to the others
        // without looking like a different suite.
        constexpr f32 kClear = 86.0f / 255.0f;

        struct SplatViewUniforms
        {
            glm::mat4 View{ 1.0f };
            glm::mat4 Projection{ 1.0f };
            glm::vec4 ViewportFocal{ 0.0f };
        };
        static_assert(sizeof(SplatViewUniforms) == 144);

        [[nodiscard]] fs::path FixturePath()
        {
            static constexpr const char* kRelative = "assets/tests/splat/fixture_discs.ply";
            const std::array<fs::path, 2> candidates{ fs::path(kRelative), fs::path("OloEditor") / kRelative };
            for (const fs::path& candidate : candidates)
            {
                if (fs::exists(candidate))
                    return candidate;
            }
            return {};
        }

        void WritePng(const std::string& name, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            // GL readback is bottom-up; PNG is top-down.
            std::vector<u8> flipped(rgba.size());
            const sizet rowBytes = static_cast<sizet>(width) * 4;
            for (u32 y = 0; y < height; ++y)
                std::copy_n(rgba.begin() + static_cast<std::ptrdiff_t>((height - 1 - y) * rowBytes), rowBytes,
                            flipped.begin() + static_cast<std::ptrdiff_t>(y * rowBytes));

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                             flipped.data(), static_cast<int>(rowBytes));
        }

        [[nodiscard]] f64 MeanAbsDifference(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return -1.0;
            f64 sum = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    sum += std::abs(static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]));
                    ++count;
                }
            }
            return count ? sum / static_cast<f64>(count) : -1.0;
        }

        // A cloud with the same statistics as the fixture but an arbitrary
        // splat count, for the scaling measurements. Deterministic.
        [[nodiscard]] SplatCloud MakeSyntheticCloud(u32 count, u32 seed)
        {
            std::mt19937 rng(seed);
            std::uniform_real_distribution<f32> unit(-1.0f, 1.0f);
            std::vector<glm::vec3> positions(count);
            std::vector<glm::vec3> shDc(count);
            std::vector<f32> opacity(count, 0.85f); // logit(0.7)
            std::vector<glm::vec3> logScale(count, glm::vec3(std::log(0.02f)));
            std::vector<glm::vec4> rotation(count, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
            for (u32 i = 0; i < count; ++i)
            {
                positions[i] = glm::vec3(unit(rng), unit(rng), unit(rng)) * 2.0f;
                shDc[i] = glm::vec3(unit(rng), unit(rng), unit(rng));
            }
            SplatCloud cloud;
            cloud.Build(positions, shDc, opacity, logScale, rotation);
            return cloud;
        }

        // Owns the GL objects one splat draw needs. Raw GL rather than the
        // engine's buffer wrappers because this is a probe outside the render
        // graph and the buffers are re-uploaded per pose.
        class SplatDrawRig
        {
          public:
            SplatDrawRig()
            {
                glCreateVertexArrays(1, &m_Vao);
                glCreateBuffers(1, &m_SplatSsbo);
                glCreateBuffers(1, &m_OrderSsbo);
            }

            ~SplatDrawRig()
            {
                glDeleteBuffers(1, &m_OrderSsbo);
                glDeleteBuffers(1, &m_SplatSsbo);
                glDeleteVertexArrays(1, &m_Vao);
            }

            SplatDrawRig(const SplatDrawRig&) = delete;
            SplatDrawRig& operator=(const SplatDrawRig&) = delete;

            void UploadCloud(const SplatCloud& cloud)
            {
                glNamedBufferData(m_SplatSsbo, static_cast<GLsizeiptr>(cloud.GpuBytes()), cloud.Splats().data(),
                                  GL_STATIC_DRAW);
            }

            void UploadOrder(const std::vector<u32>& indices)
            {
                glNamedBufferData(m_OrderSsbo, static_cast<GLsizeiptr>(indices.size() * sizeof(u32)), indices.data(),
                                  GL_STREAM_DRAW);
            }

            // `blend` selects the splat path (premultiplied over, no depth
            // write) from the opaque control (depth test and write, no blend).
            void Draw(u32 instanceCount, bool blend) const
            {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_SplatSsbo);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_OrderSsbo);
                glBindVertexArray(m_Vao);

                if (blend)
                {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glDisable(GL_DEPTH_TEST);
                    glDepthMask(GL_FALSE);
                }
                else
                {
                    glDisable(GL_BLEND);
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(GL_LEQUAL);
                    glDepthMask(GL_TRUE);
                }

                glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(instanceCount));

                glBindVertexArray(0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
                glDisable(GL_DEPTH_TEST);
            }

          private:
            u32 m_Vao = 0;
            u32 m_SplatSsbo = 0;
            u32 m_OrderSsbo = 0;
        };

        [[nodiscard]] Ref<Framebuffer> MakeTarget()
        {
            FramebufferSpecification spec;
            spec.Width = kWidth;
            spec.Height = kHeight;
            spec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
            return Framebuffer::Create(spec);
        }

        [[nodiscard]] SplatViewUniforms MakeUniforms(const glm::mat4& view, const glm::mat4& projection)
        {
            SplatViewUniforms uniforms;
            uniforms.View = view;
            uniforms.Projection = projection;
            uniforms.ViewportFocal = glm::vec4(static_cast<f32>(kWidth), static_cast<f32>(kHeight),
                                               0.5f * static_cast<f32>(kWidth) * projection[0][0],
                                               0.5f * static_cast<f32>(kHeight) * projection[1][1]);
            return uniforms;
        }

        struct Pose
        {
            const char* Name;
            glm::vec3 Eye;
            glm::vec3 Target;
            // Per-pose, because the top-down pose looks almost exactly along
            // the world up axis, where glm::lookAt's cross product degenerates
            // and returns a view matrix full of NaNs.
            glm::vec3 Up;
        };

        // Four poses chosen for what each one can falsify:
        //   Front  — the blue disc faces the camera; the red and green ones are
        //            edge-on, which only reads as a line if the covariance is
        //            anisotropic. Round blobs here mean a decode bug.
        //   Corner — all three discs at an angle; the shell overlaps every one
        //            of them, so this is where an ordering failure shows.
        //   Top    — looking down the green disc's normal.
        //   Close  — inside the shell, straddling the near plane, where the
        //            projection is least well conditioned.
        constexpr std::array<Pose, 4> kPoses{ {
            { "Front", glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
            { "Corner", glm::vec3(4.0f, 3.0f, 4.5f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
            { "Top", glm::vec3(0.0f, 6.0f, 0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f) },
            { "Close", glm::vec3(1.1f, 0.5f, 1.5f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f) },
        } };

        [[nodiscard]] glm::mat4 Projection()
        {
            return glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.05f, 100.0f);
        }
    } // namespace

    // -------------------------------------------------------------------------

    class GaussianSplatRenderTest : public ::testing::Test
    {
      protected:
        void TearDown() override
        {
            m_Rig.reset();
        }

        void SetUp() override
        {
            OLO_ENSURE_GPU_OR_SKIP();

            const fs::path path = FixturePath();
            ASSERT_FALSE(path.empty()) << "fixture_discs.ply not found from " << fs::current_path().string()
                                       << " -- run the suite from OloEditor/";
            const LoadResult result = m_Cloud.LoadPly(path);
            ASSERT_TRUE(result.Ok) << result.Error;

            m_Shader = Shader::Create("assets/shaders/tests/SplatSpike_Gaussian.glsl");
            ASSERT_TRUE(m_Shader) << "SplatSpike_Gaussian.glsl failed to compile";
            m_Ubo = UniformBuffer::Create(sizeof(SplatViewUniforms), 7);
            ASSERT_TRUE(m_Ubo);
            m_Target = MakeTarget();
            ASSERT_TRUE(m_Target);

            // Constructed HERE, not as a value member: the rig makes GL calls
            // in its constructor, and members are built before SetUp runs the
            // no-context skip.
            m_Rig = std::make_unique<SplatDrawRig>();
        }

        // Renders `cloud` with `order` and returns the RGBA8 frame.
        [[nodiscard]] std::vector<u8> RenderFrame(const SplatCloud& cloud, const std::vector<u32>& order,
                                                  const glm::mat4& view, const Ref<Shader>& shader, bool blend)
        {
            const SplatViewUniforms uniforms = MakeUniforms(view, Projection());
            m_Ubo->SetData(&uniforms, sizeof(uniforms));

            m_Rig->UploadCloud(cloud);
            m_Rig->UploadOrder(order);

            m_Target->Bind();
            glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
            glClearColor(kClear, kClear, kClear, 1.0f);
            glClearDepth(1.0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            shader->Bind();
            m_Rig->Draw(static_cast<u32>(order.size()), blend);
            m_Target->Unbind();

            std::vector<u8> pixels;
            ReadbackRgba8(m_Target->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            return pixels;
        }

        SplatCloud m_Cloud;
        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_Ubo;
        Ref<Framebuffer> m_Target;
        std::unique_ptr<SplatDrawRig> m_Rig;
    };

    TEST_F(GaussianSplatRenderTest, FixtureRendersFromEveryPoseAndTheDiscsAreDistinguishable)
    {
        for (const Pose& pose : kPoses)
        {
            const glm::mat4 view = glm::lookAt(pose.Eye, pose.Target, pose.Up);

            ViewOrdering ordering;
            BuildViewOrdering(m_Cloud, view, Projection(), glm::vec2(kWidth, kHeight), ViewSettings{}, ordering);
            ASSERT_GT(ordering.Stats.Drawn, 0u) << pose.Name << ": nothing survived culling";

            const std::vector<u8> frame = RenderFrame(m_Cloud, ordering.Indices, view, m_Shader, true);
            WritePng(std::string("Splat_") + pose.Name + ".png", frame, kWidth, kHeight);

            // Coverage: enough of the frame moved off the clear colour that the
            // capture is a picture of something. A cloud that failed to project
            // (a NaN conic, a collapsed quad) reads as an untouched frame.
            u32 painted = 0;
            u32 reddish = 0;
            u32 greenish = 0;
            u32 bluish = 0;
            const auto clearByte = static_cast<u8>(std::lround(kClear * 255.0f));
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int r = frame[i];
                const int g = frame[i + 1];
                const int b = frame[i + 2];
                const int deviation = std::max({ std::abs(r - clearByte), std::abs(g - clearByte),
                                                 std::abs(b - clearByte) });
                if (deviation > 8)
                    ++painted;
                if (r > g + 40 && r > b + 40)
                    ++reddish;
                if (g > r + 30 && g > b + 30)
                    ++greenish;
                if (b > r + 30 && b > g + 20)
                    ++bluish;
            }

            const u32 total = kWidth * kHeight;
            EXPECT_GT(painted, total / 50u) << pose.Name << ": the cloud covers almost none of the frame";

            std::printf("[splat] pose=%-7s drawn=%6u painted=%6u red=%5u green=%5u blue=%5u\n", pose.Name,
                        ordering.Stats.Drawn, painted, reddish, greenish, bluish);

            // Every disc keeps its own hue from every pose. A covariance or SH
            // decode that washed the colours together fails here; so does an
            // ordering that let the white shell cover everything.
            EXPECT_GT(reddish, 200u) << pose.Name << ": the red disc is missing";
            EXPECT_GT(greenish, 200u) << pose.Name << ": the green disc is missing";
            EXPECT_GT(bluish, 200u) << pose.Name << ": the blue disc is missing";
        }
    }

    TEST_F(GaussianSplatRenderTest, DrawOrderChangesTheImage)
    {
        // The claim under test is that back-to-front ordering is REQUIRED, not
        // cosmetic. If reversing the order produced the same pixels, the whole
        // per-frame sort would be dead weight -- and this suite would be
        // asserting nothing about it.
        const Pose& pose = kPoses[1]; // Corner: maximum overlap
        const glm::mat4 view = glm::lookAt(pose.Eye, pose.Target, pose.Up);

        ViewOrdering ordering;
        BuildViewOrdering(m_Cloud, view, Projection(), glm::vec2(kWidth, kHeight), ViewSettings{}, ordering);
        ASSERT_GT(ordering.Stats.Drawn, 0u);

        const std::vector<u8> correct = RenderFrame(m_Cloud, ordering.Indices, view, m_Shader, true);

        std::vector<u32> reversed = ordering.Indices;
        std::reverse(reversed.begin(), reversed.end());
        const std::vector<u8> wrong = RenderFrame(m_Cloud, reversed, view, m_Shader, true);
        WritePng("Splat_Corner_ReversedOrder.png", wrong, kWidth, kHeight);

        const f64 difference = MeanAbsDifference(correct, wrong);
        std::printf("[splat] reversed-order mean abs difference: %.3f / 255\n", difference);
        EXPECT_GT(difference, 1.0)
            << "front-to-back and back-to-front produced near-identical frames; the sort is not doing anything";
    }

    TEST_F(GaussianSplatRenderTest, TheSplatBudgetDropsTheDiffuseMassBeforeTheBrightDetail)
    {
        // A budget is only a useful LOD knob if the frame it produces still
        // looks like the scene. This one does not, and the captures say so:
        // ranking survivors on screen-area x alpha keeps the big opaque disc
        // splats and deletes the faint shell entirely, so the cloud loses its
        // BODY rather than its detail. That is the finding, so it is asserted
        // rather than left as a caption on a PNG -- see the ADR's section 5.
        const Pose& pose = kPoses[1];
        const glm::mat4 view = glm::lookAt(pose.Eye, pose.Target, pose.Up);

        // Splits the frame into the shell (bright, near-neutral) and the discs
        // (strongly saturated), which is what the fixture's two populations
        // look like once composited.
        const auto classify = [](const std::vector<u8>& frame, u32& shellOut, u32& discOut)
        {
            shellOut = 0;
            discOut = 0;
            const auto clearByte = static_cast<int>(std::lround(kClear * 255.0f));
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                const int r = frame[i];
                const int g = frame[i + 1];
                const int b = frame[i + 2];
                const int high = std::max({ r, g, b });
                const int low = std::min({ r, g, b });
                if (high - low > 60)
                    ++discOut;
                else if (high - clearByte > 25)
                    ++shellOut;
            }
        };

        u32 fullShell = 0;
        u32 fullDisc = 0;
        std::vector<u8> full;
        for (const u32 budget : { 4096u, 1024u, 256u })
        {
            ViewSettings settings;
            settings.MaxSplats = budget;
            ViewOrdering ordering;
            BuildViewOrdering(m_Cloud, view, Projection(), glm::vec2(kWidth, kHeight), settings, ordering);
            EXPECT_LE(ordering.Stats.Drawn, budget);

            const std::vector<u8> frame = RenderFrame(m_Cloud, ordering.Indices, view, m_Shader, true);
            WritePng("Splat_Budget_" + std::to_string(budget) + ".png", frame, kWidth, kHeight);

            u32 shell = 0;
            u32 disc = 0;
            classify(frame, shell, disc);
            std::printf("[splat] budget=%5u drawn=%5u shell-px=%6u disc-px=%6u\n", budget, ordering.Stats.Drawn,
                        shell, disc);

            if (full.empty())
            {
                full = frame;
                fullShell = shell;
                fullDisc = disc;
                ASSERT_GT(fullShell, 20000u) << "the unbudgeted frame should show a lot of shell";
                ASSERT_GT(fullDisc, 20000u);
                continue;
            }

            std::printf("[splat]              mean abs difference vs full: %.3f / 255\n",
                        MeanAbsDifference(full, frame));

            // The asymmetry IS the finding: at a quarter of the cloud the
            // shell is essentially gone while most of the disc area survives.
            EXPECT_LT(shell, fullShell / 4u) << "budget " << budget << ": the shell survived, contradicting the "
                                                                       "ADR's claim about what this ranking drops";
            EXPECT_GT(disc, fullDisc / 4u) << "budget " << budget << ": the discs went too, so the ranking is not "
                                                                     "doing what the ADR says";
        }
    }

    // -------------------------------------------------------------------------
    // Measurement. Printed, never asserted -- see the file header.
    // -------------------------------------------------------------------------

    TEST_F(GaussianSplatRenderTest, MeasureOrderingAndDrawCost)
    {
        auto baseline = Shader::Create("assets/shaders/tests/SplatSpike_OpaqueBaseline.glsl");
        ASSERT_TRUE(baseline) << "SplatSpike_OpaqueBaseline.glsl failed to compile";

        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = Projection();
        const SplatViewUniforms uniforms = MakeUniforms(view, projection);

        std::printf("\n[splat] ---- measurement (this machine, one run; not a CI gate) ----\n");
        std::printf("[splat] %10s %10s %10s %10s %10s %10s\n", "splats", "GPU-bytes", "sort-ms", "stdsort-ms",
                    "splat-ms", "opaque-ms");

        for (const u32 count : { 4096u, 100000u, 500000u })
        {
            const SplatCloud cloud = (count == 4096u) ? m_Cloud : MakeSyntheticCloud(count, 971u + count);

            // CPU: the whole per-view pass (cull + LOD + budget + radix sort).
            ViewOrdering ordering;
            BuildViewOrdering(cloud, view, projection, glm::vec2(kWidth, kHeight), ViewSettings{}, ordering);
            const auto cpuStart = std::chrono::steady_clock::now();
            constexpr int kCpuIterations = 5;
            for (int i = 0; i < kCpuIterations; ++i)
                BuildViewOrdering(cloud, view, projection, glm::vec2(kWidth, kHeight), ViewSettings{}, ordering);
            const f64 cpuMs = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - cpuStart)
                                  .count() /
                              kCpuIterations;

            // The same sort through std::sort, so the writeup can say what the
            // radix pass is worth rather than assuming it is worth something.
            std::vector<std::pair<u32, u32>> pairs(ordering.Indices.size());
            const glm::vec3 forward(-view[0][2], -view[1][2], -view[2][2]);
            const f32 forwardOffset = -view[3][2];
            for (sizet i = 0; i < pairs.size(); ++i)
            {
                const u32 index = ordering.Indices[i];
                pairs[i] = { DepthSortKey(glm::dot(forward, cloud.Splats()[index].Position) + forwardOffset), index };
            }
            const auto stdStart = std::chrono::steady_clock::now();
            std::stable_sort(pairs.begin(), pairs.end(),
                             [](const auto& a, const auto& b)
                             { return a.first > b.first; });
            const f64 stdSortMs =
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - stdStart).count();

            // GPU: the splat pass and the opaque control, timed with the same
            // query around the same instance count.
            m_Ubo->SetData(&uniforms, sizeof(uniforms));
            m_Rig->UploadCloud(cloud);
            m_Rig->UploadOrder(ordering.Indices);

            const auto timeDraw = [&](const Ref<Shader>& shader, bool blend)
            {
                u32 query = 0;
                glCreateQueries(GL_TIME_ELAPSED, 1, &query);

                m_Target->Bind();
                glViewport(0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight));
                glClearColor(kClear, kClear, kClear, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                shader->Bind();
                m_Rig->Draw(static_cast<u32>(ordering.Indices.size()), blend); // warm-up
                glFinish();

                glBeginQuery(GL_TIME_ELAPSED, query);
                shader->Bind();
                m_Rig->Draw(static_cast<u32>(ordering.Indices.size()), blend);
                glEndQuery(GL_TIME_ELAPSED);
                glFinish();
                m_Target->Unbind();

                u64 nanoseconds = 0;
                glGetQueryObjectui64v(query, GL_QUERY_RESULT, &nanoseconds);
                glDeleteQueries(1, &query);
                return static_cast<f64>(nanoseconds) / 1.0e6;
            };

            // INTERLEAVED, and reported as the MINIMUM of the samples. Two
            // separate rules, each from a real mismeasurement here:
            //   * A-then-B drifts far enough on this box to invert a
            //     comparison, so the two configurations alternate.
            //   * a first run of this test reported 10.3 ms where a second
            //     reported 4.3 ms for the identical draw, because the GPU had
            //     not reached its clock state. The minimum over several samples
            //     is the least noisy estimator of the steady-state cost; a mean
            //     just averages in the warm-up.
            f64 splatMs = std::numeric_limits<f64>::max();
            f64 opaqueMs = std::numeric_limits<f64>::max();
            for (int i = 0; i < 4; ++i)
            {
                splatMs = std::min(splatMs, timeDraw(m_Shader, true));
                opaqueMs = std::min(opaqueMs, timeDraw(baseline, false));
            }

            std::printf("[splat] %10u %10zu %10.3f %10.3f %10.3f %10.3f\n", ordering.Stats.Drawn, cloud.GpuBytes(),
                        cpuMs, stdSortMs, splatMs, opaqueMs);
        }
        std::printf("[splat] ------------------------------------------------------------\n\n");
    }
} // namespace OloEngine::Tests
