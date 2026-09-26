#include "OloEnginePCH.h"
#include "RendererStateMachineHarness.h"

#include "StateMachineCoverage.h"
#include "TestOptions.h"
#include "TestTempDir.h"
#include "TraceMinimizer.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace OloEngine::Tests::StateMachine
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr std::string_view kCaptureHookKey = "renderer-state-machine-capture";
        constexpr std::string_view kColdCacheHookKey = "renderer-state-machine-cold-bindings";
        constexpr std::string_view kCompositeName = "UIComposite";

        // The stage outputs a capture reads, each at its own pinned pass. Only
        // colour formats: an integer or depth target cannot be read back as
        // RGBA float without a GL error.
        constexpr std::array<std::string_view, 18> kTrackedTargets{
            ResourceNames::SceneColorTexture,
            ResourceNames::SceneViewNormals,
            ResourceNames::GBufferAlbedo,
            ResourceNames::GBufferNormal,
            ResourceNames::GBufferEmissive,
            ResourceNames::AOBuffer,
            ResourceNames::AOApplyColorTexture,
            ResourceNames::SSRColorTexture,
            ResourceNames::EASUColorTexture,
            ResourceNames::BloomColorTexture,
            ResourceNames::ToneMapColorTexture,
            ResourceNames::ColorGradingColorTexture,
            ResourceNames::VignetteColorTexture,
            ResourceNames::FXAAColorTexture,
            ResourceNames::UpscalerColorTexture,
            ResourceNames::SelectionOutlineColorTexture,
            ResourceNames::UICompositeTexture,
            ResourceNames::ColorBlindColorTexture,
        };

        [[nodiscard]] bool IsReadableColorFormat(GLint internalFormat)
        {
            switch (internalFormat)
            {
                case GL_RGBA8:
                case GL_SRGB8_ALPHA8:
                case GL_RGBA16F:
                case GL_RGBA32F:
                case GL_RGB16F:
                case GL_RGB32F:
                case GL_R11F_G11F_B10F:
                case GL_RGB10_A2:
                case GL_RG16F:
                case GL_RG32F:
                case GL_R16F:
                case GL_R32F:
                case GL_R8:
                case GL_RG8:
                case GL_RGB8:
                case GL_RGBA16:
                    return true;
                default:
                    return false;
            }
        }

        struct Pose
        {
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };
        constexpr std::array<Pose, kPoseCount> kPoses{ Pose{ { 0.0f, 4.0f, 12.0f }, 0.0f, 0.3f },
                                                       Pose{ { -5.0f, 3.0f, 10.0f }, -0.35f, 0.25f },
                                                       Pose{ { 4.0f, 6.0f, 9.0f }, 0.4f, 0.45f } };

        // Every pass that writes or last-reads a tracked target, found from this
        // frame's compiled lifetimes the first time the hook fires. A target is
        // read after each of them and the last read wins: that is its content
        // after the last pass that touched it AND actually ran. Pinning to one
        // declared pass does not work: writes land on versioned names
        // ("ToneMapColor@ToneMapPass", the base is import-only), and a declared
        // last reader can be culled, so its hook never fires.
        struct CaptureSession
        {
            bool Pinned = false;
            u32 Reads = 0;
            std::unordered_map<std::string, std::vector<std::string>> TargetsByPass;
            std::vector<TargetCapture> Targets;
            std::map<std::string, std::string> WhyNot; // tracked name -> last reason it was not read
        };

        // True for `name` itself and for its WriteNewVersion renames ("name@Pass").
        [[nodiscard]] bool InFamily(std::string_view resource, std::string_view name)
        {
            return resource == name || (resource.size() > name.size() && resource.starts_with(name) &&
                                        resource[name.size()] == '@');
        }

        [[nodiscard]] std::vector<std::string> AccessingPasses(const TArray64<RenderGraph::ResourceLifetime>& lifetimes,
                                                               std::string_view name)
        {
            std::vector<std::string> passes;
            const auto add = [&passes](std::string pass)
            {
                if (!pass.empty() && pass != "external" && std::ranges::find(passes, pass) == passes.end())
                    passes.push_back(std::move(pass));
            };
            for (const auto& lifetime : lifetimes)
            {
                if (!InFamily(lifetime.ResourceName.ToView(), name))
                    continue;
                if (lifetime.FirstWritePassIndex != std::numeric_limits<u32>::max())
                    add(lifetime.FirstWritePass.ToStdString());
                if (lifetime.LastReadPassIndex != std::numeric_limits<u32>::max())
                    add(lifetime.LastReadPass.ToStdString());
            }
            return passes;
        }

        void PinTargets(CaptureSession& session, RenderGraph& graph)
        {
            session.Pinned = true;
            const auto lifetimes = graph.GetResourceLifetimes();
            for (const std::string_view name : kTrackedTargets)
            {
                std::vector<std::string> passes = AccessingPasses(lifetimes, name);
                // An attachment view without accesses of its own inherits its
                // parent framebuffer's (SceneColorTexture -> SceneColor).
                if (passes.empty() && name.ends_with("Texture"))
                    passes = AccessingPasses(lifetimes, name.substr(0, name.size() - 7u));
                if (passes.empty())
                    session.WhyNot[std::string(name)] = "not accessed this frame";
                for (const std::string& pass : passes)
                    session.TargetsByPass[pass].emplace_back(name);
            }
        }

        // A read that fails drops what an EARLIER pass's read left for this
        // target: "the last read wins", and a stale earlier read standing in for
        // it would compare different passes' content between two executions, or
        // miss a difference both of them made after it. Dropped, the target is
        // reported as captured in one execution only, or skipped in both.
        void DropTarget(CaptureSession& session, const std::string& name, std::string why)
        {
            std::erase_if(session.Targets, [&name](const TargetCapture& target)
                          { return target.Name == name; });
            session.WhyNot[name] = std::move(why);
        }

        void ReadTarget(CaptureSession& session, RenderGraph& graph, const std::string& name, std::string_view pass)
        {
            const u32 texture = graph.ResolveTexture(graph.GetTextureHandle(name));
            if (texture == 0u)
            {
                DropTarget(session, name, "resolves to no texture after " + std::string(pass));
                return;
            }
            GLint width = 0;
            GLint height = 0;
            GLint samples = 0;
            GLint internalFormat = 0;
            ::glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
            ::glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
            ::glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_SAMPLES, &samples);
            ::glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
            if (width <= 0 || height <= 0 || samples > 0 || !IsReadableColorFormat(internalFormat))
            {
                DropTarget(session, name,
                           std::to_string(width) + "x" + std::to_string(height) + ", " + std::to_string(samples) +
                               " samples, format " + std::to_string(internalFormat) + " after " + std::string(pass));
                return;
            }
            auto it = std::ranges::find(session.Targets, name, &TargetCapture::Name);
            if (it == session.Targets.end())
            {
                session.Targets.emplace_back();
                it = std::prev(session.Targets.end());
            }
            it->Name = name;
            it->PinnedPass = std::string(pass);
            it->ReadOrder = ++session.Reads;
            it->Format = static_cast<u32>(internalFormat);
            it->Width = static_cast<u32>(width);
            it->Height = static_cast<u32>(height);
            ReadbackRgbaFloat(texture, it->Width, it->Height, it->Texels);
            session.WhyNot.erase(name);
        }

        [[nodiscard]] const TargetCapture* FindTarget(const FrameCapture& capture, std::string_view name)
        {
            const auto it = std::ranges::find(capture.Targets, name, &TargetCapture::Name);
            return it == capture.Targets.end() ? nullptr : &*it;
        }

        [[nodiscard]] const ControlFloor* FindControl(const std::vector<ControlFloor>& controls, std::string_view name)
        {
            const auto it = std::ranges::find(controls, name, &ControlFloor::Name);
            return it == controls.end() ? nullptr : &*it;
        }

        [[nodiscard]] bool TexelsIdentical(const TargetCapture& a, const TargetCapture& b)
        {
            return a.Width == b.Width && a.Height == b.Height && a.Texels.size() == b.Texels.size() &&
                   std::memcmp(a.Texels.data(), b.Texels.data(), a.Texels.size() * sizeof(f32)) == 0;
        }

        [[nodiscard]] std::string Extent(const TargetCapture& target)
        {
            return std::to_string(target.Width) + "x" + std::to_string(target.Height);
        }

        [[nodiscard]] std::string Joined(std::string first, const std::string& second)
        {
            if (!first.empty() && !second.empty())
                first += "; ";
            return first + second;
        }

        // Why `target`'s texels are not an RGBA readback of its own extent.
        [[nodiscard]] std::string MalformedReason(const TargetCapture& target)
        {
            const sizet expected = static_cast<sizet>(target.Width) * target.Height * 4u;
            if (expected == 0u)
                return "an empty " + Extent(target) + " target";
            if (target.Texels.size() != expected)
                return std::to_string(target.Texels.size()) + " texel values for a " + Extent(target) + " RGBA target (" +
                       std::to_string(expected) + ")";
            return {};
        }

        // The shape guard: why `a` and `b` are not two readbacks of one target.
        [[nodiscard]] std::string ShapeMismatch(const TargetCapture& a, const TargetCapture& b)
        {
            if (a.Format != b.Format)
                return "format " + std::to_string(a.Format) + " vs " + std::to_string(b.Format);
            if (a.Width != b.Width || a.Height != b.Height)
                return "size " + Extent(a) + " vs " + Extent(b);
            const std::string first = MalformedReason(a);
            const std::string second = MalformedReason(b);
            return Joined(first.empty() ? first : "first: " + first, second.empty() ? second : "second: " + second);
        }

        // The finiteness guard. Takes a well-formed target.
        [[nodiscard]] std::string NonFiniteReason(const TargetCapture& target, std::string_view where)
        {
            sizet count = 0;
            sizet first = 0;
            for (sizet i = 0; i < target.Texels.size(); ++i)
            {
                if (!std::isfinite(target.Texels[i]) && count++ == 0u)
                    first = i;
            }
            if (count == 0u)
                return {};
            const sizet texel = first / 4u;
            return std::to_string(count) + " non-finite value(s) in the " + std::string(where) + ", the first " +
                   std::to_string(target.Texels[first]) + " at texel (" + std::to_string(texel % target.Width) + ", " +
                   std::to_string(texel / target.Width) + ") channel " + "RGBA"[first % 4u];
        }

        [[nodiscard]] std::array<f64, 4> ChannelMeans(const TargetCapture& target)
        {
            std::array<f64, 4> sums{};
            const sizet texels = target.Texels.size() / 4u;
            for (sizet i = 0; i < texels; ++i)
            {
                for (sizet c = 0; c < 4u; ++c)
                    sums[c] += static_cast<f64>(target.Texels[(i * 4u) + c]);
            }
            for (f64& sum : sums)
                sum /= static_cast<f64>(std::max<sizet>(texels, 1u));
            return sums;
        }

        // The largest colour magnitude in the target: what "1% of the signal"
        // is measured against, so a dim target is not judged on a scale of 1.
        [[nodiscard]] f64 Peak(const TargetCapture& target)
        {
            f64 peak = 0.0;
            for (sizet i = 0; i < target.Texels.size(); ++i)
            {
                if (i % 4u != 3u)
                    peak = std::max(peak, std::abs(static_cast<f64>(target.Texels[i])));
            }
            return peak;
        }

        [[nodiscard]] std::array<u32, 17> LuminanceHistogram(const TargetCapture& target)
        {
            // Log-spaced so HDR and LDR targets both spread over the bins. The
            // guards keep non-finite values out; one that gets here anyway has
            // a bin of its own rather than reading as black.
            std::array<u32, 17> bins{};
            const sizet texels = target.Texels.size() / 4u;
            for (sizet i = 0; i < texels; ++i)
            {
                const f32* t = &target.Texels[i * 4u];
                const f64 luminance = (0.2126 * t[0]) + (0.7152 * t[1]) + (0.0722 * t[2]);
                if (!std::isfinite(luminance))
                {
                    ++bins[16];
                    continue;
                }
                const f64 position = (std::log2(std::max(luminance, 0.0) + 1.0e-4) + 12.0) / 16.0; // [-12, 4] -> [0, 1]
                const auto bin = static_cast<sizet>(std::clamp(position, 0.0, 0.999999) * 16.0);
                ++bins[bin];
            }
            return bins;
        }

        // Per-channel means of each tile, 4 per tile, row-major. The extent is
        // split into whole kCompareTileSize steps and the remainder spread over
        // them, so every tile is 16 to 31 texels a side: an edge sliver one
        // texel wide would be the noisiest tile and set the allowance for all.
        [[nodiscard]] std::vector<f64> TileMeans(const TargetCapture& target)
        {
            const u32 tilesX = std::max(target.Width / kCompareTileSize, 1u);
            const u32 tilesY = std::max(target.Height / kCompareTileSize, 1u);
            std::vector<f64> means(static_cast<sizet>(tilesX) * tilesY * 4u, 0.0);
            std::vector<u32> counts(static_cast<sizet>(tilesX) * tilesY, 0u);
            for (u32 y = 0; y < target.Height; ++y)
            {
                for (u32 x = 0; x < target.Width; ++x)
                {
                    const sizet tileX = static_cast<sizet>(x) * tilesX / target.Width;
                    const sizet tileY = static_cast<sizet>(y) * tilesY / target.Height;
                    const sizet tile = (tileY * tilesX) + tileX;
                    ++counts[tile];
                    const f32* t = &target.Texels[((static_cast<sizet>(y) * target.Width) + x) * 4u];
                    for (sizet c = 0; c < 4u; ++c)
                        means[(tile * 4u) + c] += static_cast<f64>(t[c]);
                }
            }
            for (sizet i = 0; i < means.size(); ++i)
                means[i] /= static_cast<f64>(std::max(counts[i / 4u], 1u));
            return means;
        }

        [[nodiscard]] u32 PoolObjectCount(RenderGraph& graph)
        {
            const auto stats = graph.GetTransientPool().GetStats();
            return stats.TexturePoolSize + stats.FramebufferPoolSize + stats.BufferPoolSize;
        }

        // Alias slots the current plan shares between two or more transients.
        // Zero means "no aliasing to switch off": the pair is vacuous here.
        [[nodiscard]] u32 SharedAliasSlots(const RenderGraph& graph)
        {
            std::map<std::pair<std::string, u32>, u32> users;
            for (const auto& entry : graph.GetTransientPlan())
            {
                if (entry.WillAllocate && entry.AliasSlot != std::numeric_limits<u32>::max())
                    ++users[{ entry.AliasGroup.ToStdString(), entry.AliasSlot }];
            }
            return static_cast<u32>(std::ranges::count_if(users, [](const auto& kv)
                                                          { return kv.second > 1u; }));
        }

        // The graph the renderer is executing. The debug runtime hands it out
        // as a const Ref; the harness installs hooks and clears the pool on it.
        [[nodiscard]] RenderGraph* ActiveGraph()
        {
            return const_cast<RenderGraph*>(RenderGraphDebugRuntime::GetActiveGraph().Raw());
        }

        [[nodiscard]] CommandBufferRenderPass* GeometryStream()
        {
            return Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry);
        }

        // RAII for a boolean lever: the harness flips several per checkpoint,
        // and a lever left on would change every later test in the process.
        class ScopedLever
        {
          public:
            ScopedLever(bool (*get)(), void (*set)(bool), bool value) : m_Set(set), m_Previous(get())
            {
                m_Set(value);
            }
            ~ScopedLever()
            {
                m_Set(m_Previous);
            }
            ScopedLever(const ScopedLever&) = delete;
            ScopedLever& operator=(const ScopedLever&) = delete;

          private:
            void (*m_Set)(bool);
            bool m_Previous;
        };

        // The composite the right way up (the readback is bottom-up); returns
        // the path written, or a note saying it could not be.
        std::string WriteCompositePng(const fs::path& path, const FrameCapture& capture)
        {
            if (capture.Composite.size() != static_cast<sizet>(capture.Width) * capture.Height * 4u || capture.Width == 0u)
                return "(no composite)";
            const sizet rowBytes = static_cast<sizet>(capture.Width) * 4u;
            std::vector<u8> flipped(capture.Composite.size());
            for (u32 y = 0; y < capture.Height; ++y)
                std::memcpy(flipped.data() + (static_cast<sizet>(y) * rowBytes),
                            capture.Composite.data() + (static_cast<sizet>(capture.Height - 1u - y) * rowBytes), rowBytes);
            const std::string file = path.string();
            if (::stbi_write_png(file.c_str(), static_cast<int>(capture.Width), static_cast<int>(capture.Height), 4,
                                 flipped.data(), static_cast<int>(rowBytes)) == 0)
                return "(PNG write failed: " + file + ")";
            return file;
        }

        [[nodiscard]] std::string DescribeFailures(const RenderGraph& graph)
        {
            std::string out;
            for (const auto& failure : graph.GetResolveFailures())
                out += failure.PassName.ToStdString() + ": " + failure.Reason.ToStdString() + "\n";
            return out;
        }
    } // namespace

    // =========================================================================
    // Comparison (pure)
    // =========================================================================

    f64 MeanShift(const TargetCapture& a, const TargetCapture& b)
    {
        const auto ma = ChannelMeans(a);
        const auto mb = ChannelMeans(b);
        f64 shift = 0.0;
        for (sizet c = 0; c < 4u; ++c)
            shift = std::max(shift, std::abs(ma[c] - mb[c]));
        return shift;
    }

    f64 HistogramDistance(const TargetCapture& a, const TargetCapture& b)
    {
        const auto ha = LuminanceHistogram(a);
        const auto hb = LuminanceHistogram(b);
        f64 l1 = 0.0;
        for (sizet i = 0; i < ha.size(); ++i)
            l1 += std::abs(static_cast<f64>(ha[i]) - static_cast<f64>(hb[i]));
        const auto texels = static_cast<f64>(std::max<sizet>(a.Texels.size() / 4u, 1u));
        return l1 / texels;
    }

    f64 TileShift(const TargetCapture& a, const TargetCapture& b)
    {
        // Tiles of different extents do not cover the same texels.
        if (a.Width != b.Width || a.Height != b.Height)
            return std::numeric_limits<f64>::infinity();
        const std::vector<f64> ta = TileMeans(a);
        const std::vector<f64> tb = TileMeans(b);
        f64 shift = 0.0;
        for (sizet i = 0; i < ta.size(); ++i)
            shift = std::max(shift, std::abs(ta[i] - tb[i]));
        return shift;
    }

    std::vector<ControlFloor> MeasureControls(const FrameCapture& first, const FrameCapture& second)
    {
        std::vector<ControlFloor> controls;
        const auto measure = [&first, &second, &controls](const std::string& name)
        {
            if (FindControl(controls, name) != nullptr)
                return;
            ControlFloor floor;
            floor.Name = name;
            const TargetCapture* a = FindTarget(first, name);
            const TargetCapture* b = FindTarget(second, name);
            if (a == nullptr || b == nullptr)
                floor.Why = std::string("captured in the ") + (a == nullptr ? "second" : "first") + " control frame only";
            else if (const std::string mismatch = ShapeMismatch(*a, *b); !mismatch.empty())
                floor.Why = "the control frames disagree in shape: " + mismatch;
            else
                floor.Why = Joined(NonFiniteReason(*a, "first control frame"), NonFiniteReason(*b, "second control frame"));
            floor.Calibrated = floor.Why.empty();
            if (!floor.Calibrated)
            {
                floor.Exact = false;
            }
            else if (floor.Exact = TexelsIdentical(*a, *b); !floor.Exact)
            {
                floor.MeanShift = MeanShift(*a, *b);
                floor.HistogramL1 = HistogramDistance(*a, *b);
                floor.TileShift = TileShift(*a, *b);
            }
            controls.push_back(std::move(floor));
        };
        // The second frame's order first: it is the one the pairs compare.
        for (const TargetCapture& target : second.Targets)
            measure(target.Name);
        for (const TargetCapture& target : first.Targets)
            measure(target.Name);
        return controls;
    }

    std::vector<ControlFloor> MergeControls(const std::vector<ControlFloor>& first, const std::vector<ControlFloor>& second)
    {
        const auto uncalibrate = [](ControlFloor& floor, std::string why)
        {
            if (floor.Calibrated)
                floor.Why = std::move(why);
            floor.Calibrated = false;
            floor.Exact = false;
        };
        std::vector<ControlFloor> merged = first;
        for (ControlFloor& floor : merged)
        {
            if (!floor.Calibrated)
                floor.Why = "first execution: " + floor.Why;
            else if (FindControl(second, floor.Name) == nullptr)
                uncalibrate(floor, "measured by the first execution's controls only");
        }
        for (const ControlFloor& other : second)
        {
            const auto it = std::ranges::find(merged, other.Name, &ControlFloor::Name);
            if (it == merged.end())
            {
                ControlFloor& added = merged.emplace_back(other);
                if (!added.Calibrated)
                    added.Why = "second execution: " + added.Why;
                uncalibrate(added, "measured by the second execution's controls only");
                continue;
            }
            if (!other.Calibrated)
                uncalibrate(*it, "second execution: " + other.Why);
            it->Exact = it->Exact && other.Exact;
            it->MeanShift = std::max(it->MeanShift, other.MeanShift);
            it->HistogramL1 = std::max(it->HistogramL1, other.HistogramL1);
            it->TileShift = std::max(it->TileShift, other.TileShift);
        }
        return merged;
    }

    Comparison CompareCaptures(const FrameCapture& a, const FrameCapture& b, const std::vector<ControlFloor>& controls)
    {
        // A mean shift of up to 0.1% of the target's magnitude (floored at
        // 1e-3) and a histogram L1 of up to 2% of the texels are indistinguishable
        // from frame noise on a stochastic target; above that, twice the
        // control's own measurement is the bar.
        constexpr f64 kMeanFloorRelative = 1.0e-3;
        constexpr f64 kHistogramFloor = 0.02;
        constexpr f64 kControlHeadroom = 2.0;
        // The spatial term: a tile mean may move by three times what the
        // control's tiles moved, or 1% of the target's peak (2.5 steps of a
        // full-range 8-bit target). Three, not two: the largest of a few
        // hundred tile differences is itself a noisy statistic, and two noisy
        // SSR frames came within 1.8x of each other on the corpus. The floor is
        // relative to the peak, not to max(1, mean), or a dim target's whole
        // signal would sit under it. Measured on the corpus (#1492): no clean
        // comparison used more than 22% of this allowance, while the stale-key
        // fault that fresh-vs-sequence catches moved a tile by twenty times it.
        constexpr f64 kTileFloorRelative = 1.0e-2;
        constexpr f64 kTileHeadroom = 3.0;

        // Everything read after the first noisy target in the frame is held at
        // distribution level too, whatever its own control says. Its input
        // moves, so a bit-stable control is a coincidence of quantisation: an
        // 8-bit composite rounded a sub-LSB wobble in its float input the same
        // way on both control frames and the other way on a third, and failed an
        // exact comparison for no reason of its own (found by this harness,
        // #1349, as a 5-texel difference that only appeared after two unrelated
        // tests had run first). Kept deliberately (#1492) rather than replaced
        // by resource lineage: with the tile term a downstream target still
        // fails any tile mean that moves by more than 1% of its peak, so what
        // the spread admits is sub-tile, sub-percent drift, and following lineage would need the
        // graph's read sets inside a pure CPU comparison.
        u32 firstNoisy = std::numeric_limits<u32>::max();
        for (const TargetCapture& target : a.Targets)
        {
            if (const ControlFloor* control = FindControl(controls, target.Name);
                control != nullptr && control->Calibrated && !control->Exact)
                firstNoisy = std::min(firstNoisy, target.ReadOrder);
        }

        Comparison out;
        // A guard failed: no statistic of this target means anything.
        const auto reject = [&out](TargetVerdict verdict, const std::string& why)
        {
            verdict.Rejected = true;
            verdict.Held = false;
            verdict.Detail += why;
            out.Held = false;
            out.Targets.push_back(std::move(verdict));
        };
        for (const TargetCapture& target : a.Targets)
        {
            TargetVerdict verdict;
            verdict.Name = target.Name;
            const TargetCapture* other = FindTarget(b, target.Name);
            if (other == nullptr)
            {
                reject(std::move(verdict), "captured in the first execution only (pinned after " + target.PinnedPass + ")");
                continue;
            }
            if (const std::string mismatch = ShapeMismatch(target, *other); !mismatch.empty())
            {
                reject(std::move(verdict), mismatch);
                continue;
            }
            if (const std::string invalid =
                    Joined(NonFiniteReason(target, "first execution"), NonFiniteReason(*other, "second execution"));
                !invalid.empty())
            {
                reject(std::move(verdict), invalid);
                continue;
            }
            const ControlFloor* control = FindControl(controls, target.Name);
            if (control == nullptr)
            {
                reject(std::move(verdict), "no control measured it, so no criterion for it exists");
                continue;
            }
            if (!control->Calibrated)
            {
                reject(std::move(verdict), "its controls calibrate nothing: " + control->Why);
                continue;
            }
            if (other->PinnedPass != target.PinnedPass)
                verdict.Detail = "pinned after " + target.PinnedPass + " vs " + other->PinnedPass + "; ";

            verdict.Exact = control->Exact && target.ReadOrder <= firstNoisy;
            const sizet texels = target.Texels.size() / 4u;
            for (sizet i = 0; i < texels; ++i)
            {
                f64 delta = 0.0;
                bool differs = false;
                for (sizet c = 0; c < 4u; ++c)
                {
                    const f32 x = target.Texels[(i * 4u) + c];
                    const f32 y = other->Texels[(i * 4u) + c];
                    if (std::memcmp(&x, &y, sizeof(f32)) != 0)
                    {
                        differs = true;
                        delta = std::max(delta, std::abs(static_cast<f64>(x) - y));
                    }
                }
                if (differs)
                {
                    ++verdict.DifferingTexels;
                    verdict.MaxDelta = std::max(verdict.MaxDelta, delta);
                }
            }

            if (verdict.Exact)
            {
                verdict.Held = verdict.DifferingTexels == 0u;
                if (!verdict.Held)
                    verdict.Detail += std::to_string(verdict.DifferingTexels) + " of " + std::to_string(texels) +
                                      " texels differ (max delta " + std::to_string(verdict.MaxDelta) +
                                      ") where the control pair was bit-identical";
            }
            else
            {
                out.AnyDistributionFallback = true;
                const f64 shift = MeanShift(target, *other);
                const f64 histogram = HistogramDistance(target, *other);
                const f64 tiles = TileShift(target, *other);
                const auto means = ChannelMeans(target);
                const f64 magnitude = std::max({ 1.0, std::abs(means[0]), std::abs(means[1]), std::abs(means[2]) });
                const f64 allowedShift = std::max(kControlHeadroom * control->MeanShift, kMeanFloorRelative * magnitude);
                const f64 allowedHistogram = std::max(kControlHeadroom * control->HistogramL1, kHistogramFloor);
                const f64 allowedTiles =
                    std::max(kTileHeadroom * control->TileShift, kTileFloorRelative * std::max(Peak(target), Peak(*other)));
                verdict.Held = shift <= allowedShift && histogram <= allowedHistogram && tiles <= allowedTiles;
                if (!verdict.Held)
                    verdict.Detail += "distribution differs (" + std::to_string(verdict.DifferingTexels) + " of " +
                                      std::to_string(texels) + " texels, max delta " + std::to_string(verdict.MaxDelta) +
                                      "): mean shift " + std::to_string(shift) + " (allowed " +
                                      std::to_string(allowedShift) + "), histogram L1 " + std::to_string(histogram) +
                                      " (allowed " + std::to_string(allowedHistogram) + "), tile shift " +
                                      std::to_string(tiles) + " (allowed " + std::to_string(allowedTiles) + ")";
            }
            if (!verdict.Held)
                out.Held = false;
            out.Targets.push_back(std::move(verdict));
        }
        for (const TargetCapture& target : b.Targets)
        {
            if (FindTarget(a, target.Name) == nullptr)
            {
                TargetVerdict verdict;
                verdict.Name = target.Name;
                reject(std::move(verdict), "captured in the second execution only (pinned after " + target.PinnedPass + ")");
            }
        }
        return out;
    }

    std::string Comparison::Describe() const
    {
        std::string out;
        u32 shown = 0;
        for (const TargetVerdict& target : Targets)
        {
            if (target.Held)
                continue;
            if (shown++ == 6u)
            {
                out += "  ...\n";
                break;
            }
            const char* criterion = target.Rejected ? " [rejected]" : (target.Exact ? " [exact]" : " [distribution]");
            out += "  " + target.Name + criterion + ": " + target.Detail + "\n";
        }
        return out;
    }

    // =========================================================================
    // Corpus
    // =========================================================================

    std::string CorpusDirectory()
    {
        return (fs::path(OLO_TEST_EDITOR_ROOT).parent_path() / "OloEngine" / "tests" / "Rendering" / "StateMachine" /
                "corpus")
            .string();
    }

    std::optional<Trace> LoadTraceFile(const std::string& path, std::string* error)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            if (error != nullptr)
                *error = "cannot open " + path;
            return std::nullopt;
        }
        std::stringstream text;
        text << file.rdbuf();
        return ParseTrace(text.str(), error);
    }

    // =========================================================================
    // Fixture
    // =========================================================================

    RendererStateMachineFixture::RendererStateMachineFixture() = default;
    RendererStateMachineFixture::~RendererStateMachineFixture() = default;

    void RendererStateMachineFixture::BuildScene()
    {
        if (!RenderPropertyFixture::IsGpuAvailable())
            return;

        m_SavedDoubleBuffering = FrameResourceManager::Get().IsDoubleBufferingEnabled();
        m_SavedVerify = Levers::VerifyDeclarationCache();
        m_SavedDisableAliasing = Levers::DisableTransientAliasing();
        m_SavedSerialSubmission = Levers::SerialMeshSubmission();
        m_SavedFaultStaleKey = Levers::FaultStaleDeclarationKey();
        m_SavedFaultBindingReset = Levers::FaultSkipDispatchBindingReset();
        m_SavedFaultShortLifetimes = Levers::FaultShortenTransientLifetimes();
        m_LeversSaved = true;

        EnableRendering(kSizes[0].Width, kSizes[0].Height);

        if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
            m_Cube = cube->GetMeshSource();
        if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
            m_Plane = plane->GetMeshSource();
        m_FieldModelPath =
            (fs::path(OLO_TEST_EDITOR_ROOT) / "assets" / "tests" / "state-machine" / "MultiMeshField40.gltf").string();
        m_FieldModel = Ref<Model>::Create(m_FieldModelPath);

        // Temporal effects accumulate, so two frames of one state are not the
        // same image and a pair could not tell a fault from accumulation. The
        // temporal test turns TAA back on in its own baseline.
        auto& post = Renderer3D::GetPostProcessSettings();
        post.TAAEnabled = false;
        post.MotionBlurEnabled = false;
        m_BaselineRenderer = Renderer3D::GetRendererSettings();
        m_BaselineRenderer.Path = RenderingPath::Forward;
        m_BaselinePost = post;

        GetScene().SetGridVisible(false);
        GetScene().SetWorldAxisHelperVisible(false);
        GetScene().SetLightGizmosVisible(false);
        GetScene().SetCameraFrustumsVisible(false);
        PopulateScene(GetScene(), false);

        // The scene the scene-swap operation renders for one frame: different
        // content, so it owns different GPU-scene records.
        m_OtherScene = Scene::Create();
        m_OtherScene->SetIs3DModeEnabled(true);
        m_OtherScene->SetGridVisible(false);
        m_OtherScene->SetWorldAxisHelperVisible(false);
        m_OtherScene->SetLightGizmosVisible(false);
        m_OtherScene->SetCameraFrustumsVisible(false);
        m_OtherScene->OnViewportResize(kSizes[0].Width, kSizes[0].Height);
        m_OtherScene->SetRenderingEnabled(true);
        {
            Entity sun = m_OtherScene->CreateEntity("OtherSun");
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(0.3f, -0.9f, 0.2f));
            light.m_Color = glm::vec3(1.0f, 0.9f, 0.8f);
            light.m_Intensity = 3.0f;
            Entity block = m_OtherScene->CreateEntity("OtherBlock");
            block.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f, 1.0f, 2.0f);
            auto& mesh = block.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = m_Cube;
            block.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.1f, 0.6f, 0.9f, 1.0f));
        }
    }

    void RendererStateMachineFixture::TearDown()
    {
        if (m_LeversSaved)
        {
            Levers::SetVerifyDeclarationCache(m_SavedVerify);
            Levers::SetDisableTransientAliasing(m_SavedDisableAliasing);
            Levers::SetSerialMeshSubmission(m_SavedSerialSubmission);
            Levers::SetFaultStaleDeclarationKey(m_SavedFaultStaleKey);
            Levers::SetFaultSkipDispatchBindingReset(m_SavedFaultBindingReset);
            Levers::SetFaultShortenTransientLifetimes(m_SavedFaultShortLifetimes);
            FrameResourceManager::Get().SetDoubleBufferingEnabled(m_SavedDoubleBuffering);
            if (auto* geometry = GeometryStream())
            {
                auto config = geometry->GetCommandBucket().GetConfig();
                config.EnableBatching = true;
                geometry->GetCommandBucket().SetConfig(config);
            }
            // The next test must not inherit a pool sized for this one's
            // aliasing policy or its fault.
            if (RenderGraph* graph = ActiveGraph())
            {
                graph->RemovePostPassHook(kCaptureHookKey);
                graph->RemovePostPassHook(kColdCacheHookKey);
                graph->GetTransientPool().Clear();
            }
            Renderer3D::RequestRenderGraphRebuild();
        }
        m_OtherScene.Reset();
        m_FieldModel.Reset();
        RendererAttachedTest::TearDown();
    }

    void RendererStateMachineFixture::PopulateScene(Scene& scene, bool withBlendedMesh)
    {
        {
            Entity sun = scene.CreateEntity("Sun");
            sun.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
            auto& light = sun.AddComponent<DirectionalLightComponent>();
            light.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
            light.m_Color = glm::vec3(1.0f);
            light.m_Intensity = 2.5f;
        }
        {
            Entity ground = scene.CreateEntity("Ground");
            auto& transform = ground.GetComponent<TransformComponent>();
            transform.Translation = { 0.0f, -1.0f, 0.0f };
            transform.Scale = { 30.0f, 1.0f, 30.0f };
            auto& mesh = ground.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Plane;
            mesh.m_MeshSource = m_Plane;
            ground.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.45f, 0.45f, 0.48f, 1.0f));
        }
        const std::array<glm::vec3, 3> positions{ glm::vec3(-3.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.5f, -1.5f),
                                                  glm::vec3(3.0f, 0.5f, 0.5f) };
        const std::array<glm::vec4, 3> colors{ glm::vec4(0.8f, 0.2f, 0.15f, 1.0f), glm::vec4(0.2f, 0.7f, 0.25f, 1.0f),
                                               glm::vec4(0.2f, 0.3f, 0.85f, 1.0f) };
        for (u32 i = 0; i < positions.size(); ++i)
        {
            Entity cube = scene.CreateEntity("Cube");
            auto& transform = cube.GetComponent<TransformComponent>();
            transform.Translation = positions[i];
            transform.Scale = glm::vec3(1.5f);
            auto& mesh = cube.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = m_Cube;
            cube.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(colors[i]);
        }
        // Eight identical cubes. Static MeshComponents reach the scene pass as
        // GPU-scene draws, not as DrawMesh packets, so these do NOT give the
        // auto-batcher work; the second model instance below does.
        for (u32 i = 0; i < 8u; ++i)
        {
            Entity cube = scene.CreateEntity("BatchCube");
            auto& transform = cube.GetComponent<TransformComponent>();
            transform.Translation = { -5.25f + (1.5f * static_cast<f32>(i)), -0.2f, 3.5f };
            transform.Scale = glm::vec3(0.6f);
            auto& mesh = cube.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = m_Cube;
            cube.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.72f, 1.0f));
        }
        // Forty meshes in one model: Model::DrawParallel hands SubmitMeshesParallel
        // a batch above its 32-mesh threshold, so serial-vs-parallel has work.
        if (m_FieldModel)
        {
            Entity field = scene.CreateEntity("MeshField");
            auto& transform = field.GetComponent<TransformComponent>();
            transform.Translation = { 0.0f, -0.3f, -5.0f };
            transform.Scale = glm::vec3(1.2f);
            field.AddComponent<ModelComponent>(m_FieldModel, m_FieldModelPath);

            // The same model again: forty DrawMesh packets identical to the
            // first forty in everything but transform, which is exactly what
            // CommandBucket::BatchCommands merges, so batch-vs-nobatch has work.
            Entity twin = scene.CreateEntity("MeshFieldTwin");
            auto& twinTransform = twin.GetComponent<TransformComponent>();
            twinTransform.Translation = { 0.0f, 1.6f, -8.0f };
            twinTransform.Scale = glm::vec3(1.2f);
            twin.AddComponent<ModelComponent>(m_FieldModel, m_FieldModelPath);
        }
        if (withBlendedMesh)
        {
            Entity pane = scene.CreateEntity("BlendedPane");
            auto& transform = pane.GetComponent<TransformComponent>();
            transform.Translation = { 0.5f, 1.2f, 1.8f };
            transform.Scale = { 3.0f, 2.0f, 0.05f };
            auto& mesh = pane.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            mesh.m_MeshSource = m_Cube;
            Material& material = pane.AddComponent<MaterialComponent>().m_Material;
            material.SetBaseColorFactor(glm::vec4(0.9f, 0.3f, 0.6f, 0.5f));
            material.SetAlphaMode(AlphaMode::Blend);
            material.SetFlag(MaterialFlag::Blend, true);
            if (&scene == &GetScene())
                m_BlendedMesh = pane;
        }
    }

    void RendererStateMachineFixture::ReloadScene()
    {
        Scene& scene = GetScene();
        std::vector<Entity> entities;
        for (const auto entity : scene.GetAllEntitiesWith<TransformComponent>())
            entities.emplace_back(entity, &scene);
        for (Entity entity : entities)
            scene.DestroyEntity(entity);
        m_BlendedMesh = {};
        PopulateScene(scene, m_Config.BlendedMesh);
    }

    void RendererStateMachineFixture::SetBlendedMesh(bool present)
    {
        if (present == static_cast<bool>(m_BlendedMesh))
            return;
        if (!present)
        {
            GetScene().DestroyEntity(m_BlendedMesh);
            m_BlendedMesh = {};
            return;
        }
        // Rebuilding the whole scene would also be a scene reload; add the one
        // entity instead, so entity-churn stays a separate operation.
        Scene& scene = GetScene();
        Entity pane = scene.CreateEntity("BlendedPane");
        auto& transform = pane.GetComponent<TransformComponent>();
        transform.Translation = { 0.5f, 1.2f, 1.8f };
        transform.Scale = { 3.0f, 2.0f, 0.05f };
        auto& mesh = pane.AddComponent<MeshComponent>();
        mesh.m_Primitive = MeshPrimitive::Cube;
        mesh.m_MeshSource = m_Cube;
        Material& material = pane.AddComponent<MaterialComponent>().m_Material;
        material.SetBaseColorFactor(glm::vec4(0.9f, 0.3f, 0.6f, 0.5f));
        material.SetAlphaMode(AlphaMode::Blend);
        material.SetFlag(MaterialFlag::Blend, true);
        m_BlendedMesh = pane;
    }

    Size RendererStateMachineFixture::CurrentSize() const
    {
        return kSizes[m_Config.SizeIndex % kSizes.size()];
    }

    EditorCamera RendererStateMachineFixture::MakeCamera() const
    {
        const Size size = CurrentSize();
        EditorCamera camera(55.0f, static_cast<f32>(size.Width) / static_cast<f32>(size.Height), 0.05f, 400.0f);
        camera.SetViewportSize(static_cast<f32>(size.Width), static_cast<f32>(size.Height));
        const Pose& pose = kPoses[m_Config.Pose % kPoseCount];
        camera.SetPose(pose.Position, pose.Yaw, pose.Pitch);
        return camera;
    }

    void RendererStateMachineFixture::RenderFrames(u32 count)
    {
        RunEditorFrames(MakeCamera(), count);
    }

    void RendererStateMachineFixture::ApplyFeature(FeatureId feature, bool on)
    {
        auto& post = Renderer3D::GetPostProcessSettings();
        switch (feature)
        {
            case FeatureId::Bloom:
                post.BloomEnabled = on;
                break;
            case FeatureId::FXAA:
                post.FXAAEnabled = on;
                break;
            case FeatureId::GTAO:
                post.ActiveAOTechnique = on ? AOTechnique::GTAO : m_BaselinePost.ActiveAOTechnique;
                post.GTAOEnabled = on;
                // The technique reaches the graph only through Apply (#771).
                Renderer3D::ApplyRendererSettings();
                break;
            case FeatureId::GTAODenoise:
                post.GTAODenoiseEnabled = on;
                post.GTAODenoisePasses = on ? std::max(m_BaselinePost.GTAODenoisePasses, 1) : m_BaselinePost.GTAODenoisePasses;
                break;
            case FeatureId::SSR:
                post.SSREnabled = on;
                break;
            case FeatureId::VignetteGrading:
                post.VignetteEnabled = on;
                post.ColorGradingEnabled = on;
                break;
            case FeatureId::Count:
                break;
        }
    }

    void RendererStateMachineFixture::RenderOtherSceneFrame()
    {
        const Size size = CurrentSize();
        m_OtherScene->OnViewportResize(size.Width, size.Height);
        RunEditorFramesOn(*m_OtherScene, MakeCamera(), 1);
    }

    void RendererStateMachineFixture::ResetToCanonical()
    {
        Levers::SetVerifyDeclarationCache(m_SavedVerify);
        Levers::SetDisableTransientAliasing(m_SavedDisableAliasing);
        Levers::SetSerialMeshSubmission(m_SavedSerialSubmission);

        Renderer3D::GetRendererSettings() = m_BaselineRenderer;
        Renderer3D::GetPostProcessSettings() = m_BaselinePost;
        Renderer3D::ApplyRendererSettings();
        FrameResourceManager::Get().SetDoubleBufferingEnabled(true);

        m_Config = ModelConfig{};
        ResizeRenderTarget(kSizes[0].Width, kSizes[0].Height);
        ReloadScene();

        if (auto* geometry = GeometryStream())
        {
            auto config = geometry->GetCommandBucket().GetConfig();
            config.EnableBatching = true;
            geometry->GetCommandBucket().SetConfig(config);
        }
        if (RenderGraph* graph = ActiveGraph())
            graph->GetTransientPool().Clear();
        Renderer3D::RequestRenderGraphRebuild();
        (void)Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::Manual);
        auto& frames = FrameResourceManager::Get();
        for (u32 i = 0; i < FrameResourceManager::NUM_BUFFERED_FRAMES; ++i)
            frames.WaitForFrame(i);
        frames.FlushAllDeletionQueues();
    }

    void RendererStateMachineFixture::ConfigureDirectly(const ModelConfig& config)
    {
        ResetToCanonical();
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = static_cast<RenderingPath>(config.Path % kPathCount);
        settings.Deferred.MSAASampleCount = kMsaaSamples[config.MsaaIndex % kMsaaSamples.size()];
        Renderer3D::GetPostProcessSettings().Upscale =
            static_cast<UpscaleMode>(kUpscaleModes[config.UpscaleIndex % kUpscaleModes.size()]);
        for (u32 feature = 0; feature < static_cast<u32>(FeatureId::Count); ++feature)
            ApplyFeature(static_cast<FeatureId>(feature), config.HasFeature(static_cast<FeatureId>(feature)));
        Renderer3D::ApplyRendererSettings();
        const Size size = kSizes[config.SizeIndex % kSizes.size()];
        ResizeRenderTarget(size.Width, size.Height);
        FrameResourceManager::Get().SetDoubleBufferingEnabled(config.DoubleBuffering);
        m_Config = config;
        SetBlendedMesh(config.BlendedMesh);
    }

    void RendererStateMachineFixture::ApplyOp(const Op& op)
    {
        auto& settings = Renderer3D::GetRendererSettings();
        auto& post = Renderer3D::GetPostProcessSettings();
        const ModelConfig next = Apply(m_Config, op);
        switch (op.Kind)
        {
            case OpKind::Resize:
            {
                const Size size = kSizes[next.SizeIndex];
                ResizeRenderTarget(size.Width, size.Height);
                break;
            }
            case OpKind::Path:
                settings.Path = static_cast<RenderingPath>(next.Path);
                Renderer3D::ApplyRendererSettings();
                break;
            case OpKind::Feature:
                ApplyFeature(static_cast<FeatureId>((op.Arg / 2u) % static_cast<u32>(FeatureId::Count)), (op.Arg % 2u) != 0u);
                break;
            case OpKind::Msaa:
                settings.Deferred.MSAASampleCount = kMsaaSamples[next.MsaaIndex];
                Renderer3D::ApplyRendererSettings();
                break;
            case OpKind::Upscale:
                post.Upscale = static_cast<UpscaleMode>(kUpscaleModes[next.UpscaleIndex]);
                break;
            case OpKind::ShaderReload:
                Renderer3D::GetShaderLibrary().ReloadShaders();
                break;
            case OpKind::SceneReload:
                ReloadScene();
                break;
            case OpKind::EntityChurn:
                SetBlendedMesh(next.BlendedMesh);
                break;
            case OpKind::FenceDrain:
            {
                auto& frames = FrameResourceManager::Get();
                for (u32 i = 0; i < FrameResourceManager::NUM_BUFFERED_FRAMES; ++i)
                    frames.WaitForFrame(i);
                frames.FlushAllDeletionQueues();
                break;
            }
            case OpKind::FramesInFlight:
                FrameResourceManager::Get().SetDoubleBufferingEnabled(next.DoubleBuffering);
                break;
            case OpKind::SceneSwap:
                RenderOtherSceneFrame();
                break;
            case OpKind::HistoryAdvance:
            {
                (void)Renderer3D::InvalidateTemporalHistories(TemporalHistoryInvalidationCause::Manual);
                // Frames from the OTHER poses: the history advances through
                // motion, then the settle frames return to the current pose.
                const u32 home = m_Config.Pose;
                for (u32 step = 1; step < kPoseCount; ++step)
                {
                    m_Config.Pose = (home + step) % kPoseCount;
                    RenderFrames(1);
                }
                m_Config.Pose = home;
                break;
            }
            case OpKind::CameraMove:
                break; // the camera is rebuilt from the configuration every frame
            case OpKind::PoolTrim:
                if (RenderGraph* graph = ActiveGraph())
                    graph->GetTransientPool().Clear();
                break;
            case OpKind::Count:
                break;
        }
        m_Config = next;
    }

    FrameCapture RendererStateMachineFixture::CaptureFrame()
    {
        FrameCapture capture;
        RenderGraph* graph = ActiveGraph();
        if (!graph)
        {
            ADD_FAILURE() << "no active render graph to capture";
            return capture;
        }
        auto session = std::make_shared<CaptureSession>();
        graph->AddPostPassHook(kCaptureHookKey,
                               [session](std::string_view pass, RenderGraph& g)
                               {
                                   if (!session->Pinned)
                                       PinTargets(*session, g);
                                   if (const auto it = session->TargetsByPass.find(std::string(pass));
                                       it != session->TargetsByPass.end())
                                   {
                                       for (const std::string& name : it->second)
                                           ReadTarget(*session, g, name, pass);
                                   }
                               });
        RenderFrames(1);
        graph->RemovePostPassHook(kCaptureHookKey);
        capture.ResolveFailures = DescribeFailures(*graph);

        u32 width = 0;
        u32 height = 0;
        if (ReadbackComposite(capture.Composite, width, height))
        {
            capture.Width = width;
            capture.Height = height;
            TargetCapture composite;
            composite.Name = std::string(kCompositeName);
            composite.PinnedPass = "end of frame";
            composite.ReadOrder = std::numeric_limits<u32>::max();
            composite.Format = GL_RGBA8;
            composite.Width = width;
            composite.Height = height;
            composite.Texels.resize(capture.Composite.size());
            for (sizet i = 0; i < capture.Composite.size(); ++i)
                composite.Texels[i] = static_cast<f32>(capture.Composite[i]) / 255.0f;
            capture.Targets.push_back(std::move(composite));
        }
        for (TargetCapture& target : session->Targets)
            capture.Targets.push_back(std::move(target));
        for (const auto& [name, why] : session->WhyNot)
            capture.Skipped += name + " (" + why + "); ";
        std::ranges::sort(capture.Targets, {}, &TargetCapture::Name);
        return capture;
    }

    void RendererStateMachineFixture::CheckPairs(const std::string& where, const RunOptions& options, TraceResult& result)
    {
        const auto wants = [&options](std::string_view id)
        { return options.OnlyPair.empty() || options.OnlyPair == id; };
        const auto fail = [&result, &where](std::string_view id, std::string detail)
        { result.Failures.push_back(PairFailure{ std::string(id), where, std::move(detail) }); };
        const auto judge = [&fail, &result](std::string_view id, const FrameCapture& a, const FrameCapture& b,
                                            const std::vector<ControlFloor>& floors)
        {
            const Comparison comparison = CompareCaptures(a, b, floors);
            Coverage::RecordComparison(id, comparison.AnyDistributionFallback);
            if (comparison.Held)
                return;
            // Both composites, so the difference can be looked at rather than
            // only counted. Keyed by the failure's ordinal: a pair can fail at
            // several checkpoints of one trace.
            const std::string stem = std::string(id) + "-" + std::to_string(result.Failures.size());
            const std::string first = WriteCompositePng(TempFile(stem + "-a.png"), a);
            const std::string second = WriteCompositePng(TempFile(stem + "-b.png"), b);
            // Which stages were compared, and where each was read: the first
            // one in frame order that differs is where to look.
            std::string captured;
            for (const TargetCapture& target : a.Targets)
                captured += target.Name + "@" + target.PinnedPass + " ";
            fail(id, comparison.Describe() + "  composites: " + first + " vs " + second + "\n  captured: " + captured +
                         "\n");
        };

        ++result.Checkpoints;
        // A replay narrowed to a pair this function does not own (the end-of-
        // trace fresh-vs-sequence, the minimiser's commonest predicate) skips
        // the control captures entirely: twelve rendered frames and every
        // target read back twice, for nothing.
        static constexpr std::array<std::string_view, 9> kOwnedPairs{
            "cached-vs-rebuild.gl",
            "cached-vs-rebuild.plan",
            "alias-vs-noalias.gl",
            "batch-vs-nobatch.gl",
            "serial-vs-parallel.gl",
            "binding-cache-cold.gl",
            "harness.round-trip",
            "harness.capture",
            "resolve-failures",
        };
        if (!options.OnlyPair.empty() && std::ranges::find(kOwnedPairs, options.OnlyPair) == kOwnedPairs.end())
            return;
        // The control must span as many frames as the comparisons below do,
        // or a target that is still converging (a temporally resolved SSR
        // after its history was reset) reads as bit-stable over two frames and
        // then "fails" a pair captured six frames later -- together with the
        // lever-free round trip, which is the tell that it was drift, not the
        // lever. So the two control captures are kControlSpan frames apart.
        constexpr u32 kControlSpan = 10u;
        const FrameCapture first = CaptureFrame();
        RenderFrames(kControlSpan);
        const FrameCapture control = CaptureFrame();
        if (!control.ResolveFailures.empty())
            fail("resolve-failures", "the settled frame failed to resolve graph handles:\n" + control.ResolveFailures);
        if (FindTarget(control, kCompositeName) == nullptr)
            fail("harness.capture", "the composite could not be read back");
        const std::vector<ControlFloor> controls = MeasureControls(first, control);
        RenderGraph* graph = ActiveGraph();

        // --- cached vs forced rebuild ----------------------------------------
        if (wants("cached-vs-rebuild.gl") || wants("cached-vs-rebuild.plan"))
        {
            const auto before = Renderer3D::GetFrameGraphDeclarationStats();
            FrameCapture rebuilt;
            {
                const ScopedLever verify(&Levers::VerifyDeclarationCache, &Levers::SetVerifyDeclarationCache, true);
                rebuilt = CaptureFrame();
            }
            const auto after = Renderer3D::GetFrameGraphDeclarationStats();
            if (after.VerifiedHits != before.VerifiedHits + 1u)
            {
                fail("cached-vs-rebuild.plan",
                     "the settled frame was not served from the declaration cache, so the verifier compared nothing. A "
                     "settled state that still compiles every frame is itself over-invalidation; last compile cause: " +
                         after.LastCompileCause);
            }
            else
            {
                Coverage::RecordComparison("cached-vs-rebuild.plan");
                if (after.StaleCacheDetections != before.StaleCacheDetections)
                    fail("cached-vs-rebuild.plan", "stale cache: " + after.LastStaleCacheDetail);
            }
            if (wants("cached-vs-rebuild.gl"))
                judge("cached-vs-rebuild.gl", control, rebuilt, controls);
        }

        // --- transient aliasing on vs off --------------------------------------
        if (wants("alias-vs-noalias.gl") && graph)
        {
            const u32 shared = SharedAliasSlots(*graph);
            const u32 aliasedObjects = PoolObjectCount(*graph);
            FrameCapture unaliased;
            u32 unaliasedObjects = 0;
            {
                const ScopedLever noAlias(&Levers::DisableTransientAliasing, &Levers::SetDisableTransientAliasing, true);
                graph->GetTransientPool().Clear();
                RenderFrames(2);
                unaliased = CaptureFrame();
                unaliasedObjects = PoolObjectCount(*graph);
            }
            graph->GetTransientPool().Clear();
            RenderFrames(2);
            if (shared == 0u)
            {
                Coverage::RecordVacuous("alias-vs-noalias.gl");
            }
            else if (unaliasedObjects <= aliasedObjects)
            {
                fail("alias-vs-noalias.gl", "the plan shares " + std::to_string(shared) +
                                                " alias slot(s), yet disabling aliasing did not grow the pool (" +
                                                std::to_string(aliasedObjects) + " -> " + std::to_string(unaliasedObjects) +
                                                " objects): the lever did not act");
            }
            else
            {
                judge("alias-vs-noalias.gl", control, unaliased, controls);
            }
        }

        // --- auto-batching on vs off -------------------------------------------
        if (wants("batch-vs-nobatch.gl"))
        {
            if (auto* geometry = GeometryStream())
            {
                CommandBucket& bucket = geometry->GetCommandBucket();
                const u32 batchedOn = bucket.GetStatistics().BatchedCommands;
                auto config = bucket.GetConfig();
                config.EnableBatching = false;
                bucket.SetConfig(config);
                const FrameCapture unbatched = CaptureFrame();
                const u32 batchedOff = bucket.GetStatistics().BatchedCommands;
                config.EnableBatching = true;
                bucket.SetConfig(config);
                RenderFrames(1);
                if (batchedOn == 0u)
                    Coverage::RecordVacuous("batch-vs-nobatch.gl");
                else if (batchedOff != 0u)
                    fail("batch-vs-nobatch.gl", "batching off still batched " + std::to_string(batchedOff) +
                                                    " command(s): the lever did not act");
                else
                    judge("batch-vs-nobatch.gl", control, unbatched, controls);
            }
        }

        // --- parallel vs serial mesh submission --------------------------------
        if (wants("serial-vs-parallel.gl"))
        {
            const u32 parallelOn = Renderer3D::GetStats().ParallelSubmittedMeshes;
            FrameCapture serial;
            u32 parallelOff = 0;
            {
                const ScopedLever serialLever(&Levers::SerialMeshSubmission, &Levers::SetSerialMeshSubmission, true);
                serial = CaptureFrame();
                parallelOff = Renderer3D::GetStats().ParallelSubmittedMeshes;
            }
            RenderFrames(1);
            if (parallelOn == 0u)
                Coverage::RecordVacuous("serial-vs-parallel.gl");
            else if (parallelOff != 0u)
                fail("serial-vs-parallel.gl", std::to_string(parallelOff) +
                                                  " mesh(es) still went through the worker branch: the lever did not act");
            else
                judge("serial-vs-parallel.gl", control, serial, controls);
        }

        // --- warm vs cold binding caches ---------------------------------------
        // The dispatcher skips a bind it believes is already in place. Forget
        // every such belief before the frame and after every pass: if the frame
        // changes, some pass relied on a binding nobody reset.
        if (wants("binding-cache-cold.gl") && graph)
        {
            graph->AddPostPassHook(kColdCacheHookKey, [](std::string_view, RenderGraph&)
                                   { CommandDispatch::InvalidateBindingCaches(); });
            CommandDispatch::InvalidateBindingCaches();
            const FrameCapture cold = CaptureFrame();
            graph->RemovePostPassHook(kColdCacheHookKey);
            RenderFrames(1);
            judge("binding-cache-cold.gl", control, cold, controls);
        }

        // --- the levers put back reproduce the frame ---------------------------
        if (options.OnlyPair.empty() || options.OnlyPair == "harness.round-trip")
        {
            const FrameCapture restored = CaptureFrame();
            const Comparison comparison = CompareCaptures(control, restored, controls);
            if (!comparison.Held)
                fail("harness.round-trip", "every lever was restored, yet the frame changed:\n" + comparison.Describe());
        }
    }

    void RendererStateMachineFixture::CheckFreshVersusSequence(const std::string& where, const RunOptions& options,
                                                               TraceResult& result)
    {
        if (!options.OnlyPair.empty() && options.OnlyPair != "fresh-vs-sequence.gl")
            return;
        const FrameCapture sequenceFirst = CaptureFrame();
        const FrameCapture sequence = CaptureFrame();
        const ModelConfig target = m_Config;

        ConfigureDirectly(target);
        RenderFrames(options.SettleFrames);
        const FrameCapture freshFirst = CaptureFrame();
        const FrameCapture fresh = CaptureFrame();

        // A target is held exact only if BOTH executions' controls are exact.
        const std::vector<ControlFloor> controls =
            MergeControls(MeasureControls(sequenceFirst, sequence), MeasureControls(freshFirst, fresh));
        const Comparison comparison = CompareCaptures(sequence, fresh, controls);
        Coverage::RecordComparison("fresh-vs-sequence.gl", comparison.AnyDistributionFallback);
        if (!comparison.Held)
            result.Failures.push_back(PairFailure{ "fresh-vs-sequence.gl", where,
                                                   "the state a sequence reached renders differently from the same "
                                                   "configuration reached directly (" +
                                                       DescribeConfig(target) + "):\n" + comparison.Describe() });
    }

    TraceResult RendererStateMachineFixture::RunTrace(const Trace& trace, const RunOptions& options)
    {
        TraceResult result;
        ConfigureDirectly(trace.Initial);
        RenderFrames(options.SettleFrames);
        CheckPairs("initial state (" + DescribeConfig(trace.Initial) + ")", options, result);
        if (options.StopAtFirstFailure && !result.Passed())
            return result;

        for (sizet i = 0; i < trace.Ops.size(); ++i)
        {
            ApplyOp(trace.Ops[i]);
            RenderFrames(options.SettleFrames);
            CheckPairs("after op " + std::to_string(i + 1u) + "/" + std::to_string(trace.Ops.size()) + " '" +
                           ToString(trace.Ops[i]) + "'",
                       options, result);
            if (options.StopAtFirstFailure && !result.Passed())
                return result;
        }
        CheckFreshVersusSequence("end of trace", options, result);
        return result;
    }

    bool RendererStateMachineFixture::TraceStillFails(const Trace& trace, const std::string& pairId)
    {
        RunOptions options;
        // The harness's own checks (the lever round trip, resolve failures,
        // capture) only fail when the pairs around them flip levers, so a
        // failure of one of them is replayed with every pair on.
        const bool harnessCheck = pairId.starts_with("harness.") || pairId == "resolve-failures";
        options.OnlyPair = harnessCheck ? std::string{} : pairId;
        const TraceResult result = RunTrace(trace, options);
        return std::ranges::any_of(result.Failures, [&pairId](const PairFailure& failure)
                                   { return failure.PairId == pairId; });
    }

    bool RendererStateMachineFixture::RunTraceAndReport(const Trace& trace, const std::string& label)
    {
        const TraceResult result = RunTrace(trace, RunOptions{});
        if (result.Passed())
            return true;

        const PairFailure& first = result.Failures.front();
        const std::string original = Serialize(trace);
        const fs::path originalPath = TempFile(label + ".trace");
        {
            std::ofstream out(originalPath, std::ios::binary);
            out << "# " << label << ": pair " << first.PairId << " failed " << first.Where << "\n"
                << original;
        }

        std::string minimisedReport = "  (minimisation off: --olo-state-machine-minimize-budget=0)\n";
        if (const u32 budget = Options().StateMachineMinimizeBudget; budget > 0u)
        {
            const std::function<bool(const std::vector<Op>&)> fails = [this, &trace, &first](const std::vector<Op>& ops)
            {
                Trace candidate = trace;
                candidate.Ops = ops;
                return TraceStillFails(candidate, first.PairId);
            };
            const MinimizeResult<Op> minimised = MinimizeTrace(trace.Ops, fails, budget);
            Trace reduced = trace;
            reduced.Ops = minimised.Trace;
            const fs::path reducedPath = TempFile(label + ".min.trace");
            {
                std::ofstream out(reducedPath, std::ios::binary);
                out << "# minimised from " << label << " against pair " << first.PairId << "\n"
                    << Serialize(reduced);
            }
            minimisedReport = "Minimised to " + std::to_string(reduced.Ops.size()) + " op(s) in " +
                              std::to_string(minimised.Evaluations) + " replay(s)" +
                              (minimised.InputDidNotFail
                                   ? " -- but the trace did NOT fail again when replayed from the canonical reset, so the "
                                     "failure depends on state the reset does not restore; replay it in a fresh process"
                                   : (minimised.OneMinimal ? ", 1-minimal" : ", budget exhausted before 1-minimal")) +
                              " (" + reducedPath.string() + "):\n" + Serialize(reduced);
        }

        std::string all;
        for (const PairFailure& failure : result.Failures)
            all += "  " + failure.PairId + " " + failure.Where + "\n";
        ADD_FAILURE() << label << ": pair '" << first.PairId << "' failed " << first.Where << "\n"
                      << first.Detail << "Failures:\n"
                      << all << "Trace (" << originalPath.string() << "):\n"
                      << original << minimisedReport
                      << "Replay: OloEngine-Tests --gtest_filter=RendererStateMachineEvidence.ReplayTraceFromCommandLine "
                         "--olo-state-machine-replay=<trace> --olo-keep-temp";
        return false;
    }
} // namespace OloEngine::Tests::StateMachine
