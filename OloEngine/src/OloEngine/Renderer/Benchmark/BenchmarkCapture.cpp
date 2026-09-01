#include "OloEnginePCH.h"
#include "BenchmarkCapture.h"

#include "OloEngine/Core/Environment.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"

#include <nlohmann/json.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace OloEngine::Benchmark
{
    namespace
    {
        const char* FormatName(RGResourceFormat format)
        {
            switch (format)
            {
                case RGResourceFormat::Unknown:
                    return "Unknown";
                case RGResourceFormat::R8UNorm:
                    return "R8UNorm";
                case RGResourceFormat::R32Float:
                    return "R32Float";
                case RGResourceFormat::RG16Float:
                    return "RG16Float";
                case RGResourceFormat::RGBA8UNorm:
                    return "RGBA8UNorm";
                case RGResourceFormat::RGBA16Float:
                    return "RGBA16Float";
                case RGResourceFormat::RGBA32Float:
                    return "RGBA32Float";
                case RGResourceFormat::Depth24Stencil8:
                    return "Depth24Stencil8";
                case RGResourceFormat::Depth32Float:
                    return "Depth32Float";
                case RGResourceFormat::R32Int:
                    return "R32Int";
            }
            return "Unknown";
        }

        // The one resolve every render diagnostic uses (mirrors the editor's
        // ResolveTargetHandle, McpToolsRender.cpp — graph texture first, then
        // framebuffer colour attachment 0, then the depth attachment).
        RHI::ResourceHandle ResolveTargetHandle(const std::string& name, bool& outDepthFromFramebuffer)
        {
            outDepthFromFramebuffer = false;

            RHI::ResourceHandle handle = Renderer3D::ResolveFrameGraphTextureHandle(name);
            if (handle.IsValid())
            {
                return handle;
            }

            const Ref<Framebuffer> framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(name);
            if (!framebuffer)
            {
                return {};
            }

            handle = framebuffer->GetColorAttachmentHandle(0);
            if (handle.IsValid())
            {
                return handle;
            }

            handle = framebuffer->GetDepthAttachmentHandle();
            outDepthFromFramebuffer = handle.IsValid();
            return handle;
        }

        void AppendToVector(void* context, void* data, int size)
        {
            auto* out = static_cast<std::vector<u8>*>(context);
            const auto* bytes = static_cast<const u8*>(data);
            out->insert(out->end(), bytes, bytes + static_cast<sizet>(size));
        }

        // Neither PNG (comp=2 is grey+alpha) nor Radiance .hdr has a meaningful
        // 2-component layout — widen RG to RGB with a zeroed blue lane, once,
        // for both encode paths.
        template<typename T>
        void WidenRGToRGB(std::vector<T>& data, sizet texels)
        {
            std::vector<T> widened(texels * 3u, T{});
            for (sizet i = 0; i < texels; ++i)
            {
                widened[i * 3 + 0] = data[i * 2 + 0];
                widened[i * 3 + 1] = data[i * 2 + 1];
            }
            data = std::move(widened);
        }
    } // namespace

    CapturedAttachment CaptureAttachment(const ManifestAttachment& spec, const CaptureContext& context)
    {
        CapturedAttachment result;
        result.Spec = spec;

        const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
        if (!graph)
        {
            result.Error = "no active render graph";
            return result;
        }

        bool depthFromFramebuffer = false;
        const RHI::ResourceHandle handle = ResolveTargetHandle(spec.Source, depthFromFramebuffer);
        if (!handle.IsValid())
        {
            result.Error = "unknown render-graph resource or no GPU backing this frame: " + spec.Source;
            return result;
        }

        // Channels / depth-ness from the graph's registered format (same
        // contract as olo_render_capture_target — a handle that resolved
        // outside the registry defaults to a 4-channel float read).
        i32 channels = 4;
        bool isDepth = depthFromFramebuffer;
        const auto* resource = graph->FindRegisteredResource(spec.Source);
        const RGResourceFormat rgFormat = resource != nullptr ? resource->Desc.Format : RGResourceFormat::Unknown;
        switch (rgFormat)
        {
            case RGResourceFormat::R8UNorm:
            case RGResourceFormat::R32Float:
                channels = 1;
                break;
            case RGResourceFormat::RG16Float:
                channels = 2;
                break;
            case RGResourceFormat::RGBA8UNorm:
            case RGResourceFormat::RGBA16Float:
            case RGResourceFormat::RGBA32Float:
                channels = 4;
                break;
            case RGResourceFormat::Depth24Stencil8:
            case RGResourceFormat::Depth32Float:
                channels = 1;
                isDepth = true;
                break;
            case RGResourceFormat::R32Int:
                result.Error = "integer targets are not image-capturable: " + spec.Source;
                return result;
            case RGResourceFormat::Unknown:
                break;
        }
        result.FormatName = FormatName(rgFormat);
        result.IsDepth = isDepth;

        u32 width = 0;
        u32 height = 0;
        RenderCommand::GetTextureDimensions(handle, 0, width, height);
        if (width == 0 || height == 0)
        {
            result.Error = "texture has no storage: " + spec.Source;
            return result;
        }

        // A DEPTH source must name a DEPTH destination (GL: only depth
        // destinations lower to GL_DEPTH_COMPONENT; Vulkan: the identity fast
        // path needs the real format) — same rule as the MCP capture facade.
        const RHI::Format destFormat = isDepth         ? RHI::Format::D32Float
                                       : channels == 1 ? RHI::Format::R32Float
                                       : channels == 2 ? RHI::Format::RG32Float
                                                       : RHI::Format::RGBA32Float;
        // Exactly the channel count destFormat encodes, by construction.
        const i32 readChannels = isDepth ? 1 : channels;

        sizet valueCount = static_cast<sizet>(width) * height * static_cast<sizet>(readChannels);
        std::vector<f32> values(valueCount);
        if (!RenderCommand::ReadTextureSubImage(handle, 0, 0, 0, 0, width, height, 1u, destFormat,
                                                values.size() * sizeof(f32), values.data()))
        {
            result.Error = "readback failed (format " + result.FormatName + "): " + spec.Source;
            return result;
        }

        // CPU-side derivation (issue #974): AOVs that live as LANES of
        // existing targets, or need a transform, are produced here from the
        // exact texels just read — no render-graph pass, no shipping-frame
        // change, backend-neutral. The derived buffer is single-channel and
        // the min/max below then describes the DERIVED values.
        i32 readChannelsAfterDerive = readChannels;
        if (spec.Derive != AttachmentDerive::None)
        {
            const sizet texels = static_cast<sizet>(width) * height;
            std::vector<f32> derived(texels);
            if (spec.Derive == AttachmentDerive::LinearDepth)
            {
                if (!isDepth)
                {
                    result.Error = "Derive: linear-depth needs a depth source, got " + result.FormatName + ": " +
                                   spec.Source;
                    return result;
                }
                // Standard (non-reversed) depth: view-space metres from the
                // capturing camera's planes.
                const f32 nearClip = context.CameraNearClip;
                const f32 farClip = context.CameraFarClip;
                for (sizet i = 0; i < texels; ++i)
                {
                    const f32 d = std::clamp(values[i], 0.0f, 1.0f);
                    derived[i] = (nearClip * farClip) / (farClip - d * (farClip - nearClip));
                }
            }
            else
            {
                const i32 lane = spec.Derive == AttachmentDerive::ChannelR   ? 0
                                 : spec.Derive == AttachmentDerive::ChannelG ? 1
                                 : spec.Derive == AttachmentDerive::ChannelB ? 2
                                                                             : 3;
                if (lane >= readChannels)
                {
                    result.Error = "Derive: channel " + std::to_string(lane) + " out of range for " +
                                   result.FormatName + " (" + std::to_string(readChannels) + " channels): " +
                                   spec.Source;
                    return result;
                }
                for (sizet i = 0; i < texels; ++i)
                {
                    derived[i] = values[i * readChannels + lane];
                }
            }
            values = std::move(derived);
            readChannelsAfterDerive = 1;
            valueCount = texels;
        }

        // Finite min/max — reported for every capture, used for normalization.
        // A UNIFORM image (cleared depth, the white furnace) still reports its
        // constant value; only a fully non-finite readback is an error.
        f32 minV = std::numeric_limits<f32>::max();
        f32 maxV = std::numeric_limits<f32>::lowest();
        bool anyFinite = false;
        for (const f32 v : values)
        {
            if (std::isfinite(v))
            {
                minV = std::min(minV, v);
                maxV = std::max(maxV, v);
                anyFinite = true;
            }
        }
        if (!anyFinite)
        {
            result.Error = "readback contained no finite values: " + spec.Source;
            return result;
        }
        result.MinValue = minV;
        result.MaxValue = maxV;
        const bool haveRange = maxV > minV;

        // ONE row order per backend: off-screen targets are bottom-up under GL,
        // top-down under Vulkan; PNG/HDR are top-down.
        const bool flipRows = RHI::RenderTargetRowsAreBottomUp();
        // In place with a single row of scratch — a full-buffer copy would
        // transiently double the capture's peak heap (a 4K RGBA32F attachment
        // is ~132 MB, and this runs on the render thread in the editor host).
        const auto flipFloatRows = [&](std::vector<f32>& data, i32 comps)
        {
            if (!flipRows)
            {
                return;
            }
            const sizet rowValues = static_cast<sizet>(width) * comps;
            std::vector<f32> scratch(rowValues);
            for (sizet y = 0; y < static_cast<sizet>(height) / 2u; ++y)
            {
                f32* top = data.data() + y * rowValues;
                f32* bottom = data.data() + (static_cast<sizet>(height) - 1 - y) * rowValues;
                std::memcpy(scratch.data(), top, rowValues * sizeof(f32));
                std::memcpy(top, bottom, rowValues * sizeof(f32));
                std::memcpy(bottom, scratch.data(), rowValues * sizeof(f32));
            }
        };

        result.Width = width;
        result.Height = height;

        if (spec.Format == AttachmentFormat::Hdr)
        {
            // Full-float Radiance export — the path the PNG clamp cannot
            // provide. Never normalized: the point is the real values.
            i32 outChannels = readChannelsAfterDerive;
            if (readChannelsAfterDerive == 2)
            {
                WidenRGToRGB(values, static_cast<sizet>(width) * height);
                outChannels = 3;
            }
            flipFloatRows(values, outChannels);
            std::vector<u8> encoded;
            if (stbi_write_hdr_to_func(AppendToVector, &encoded, static_cast<int>(width), static_cast<int>(height),
                                       outChannels, values.data()) == 0)
            {
                result.Error = "HDR encode failed: " + spec.Source;
                return result;
            }
            result.FileBytes = std::move(encoded);
            result.FileName = spec.Name + ".hdr";
            return result;
        }

        // PNG: min-max normalize when asked (or Auto on a depth source), then
        // quantise to 8-bit — identical maths to olo_render_capture_target.
        const bool wantNormalize = spec.Normalize == AttachmentNormalize::On ||
                                   (spec.Normalize == AttachmentNormalize::Auto && isDepth);
        const bool doNormalize = wantNormalize && haveRange;
        result.Normalized = doNormalize;
        const f32 scale = doNormalize ? 1.0f / (maxV - minV) : 1.0f;
        const f32 bias = doNormalize ? -minV : 0.0f;

        flipFloatRows(values, readChannelsAfterDerive);

        std::vector<u8> pixels8(valueCount);
        for (sizet i = 0; i < valueCount; ++i)
        {
            const f32 safe = std::isnan(values[i]) ? 0.0f : values[i];
            pixels8[i] = static_cast<u8>(std::clamp((safe + bias) * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        i32 outChannels = readChannelsAfterDerive;
        const sizet texelCount = static_cast<sizet>(width) * height;
        if (readChannelsAfterDerive == 2)
        {
            WidenRGToRGB(pixels8, texelCount);
            outChannels = 3;
        }

        std::vector<u8> encoded;
        if (stbi_write_png_to_func(AppendToVector, &encoded, static_cast<int>(width), static_cast<int>(height),
                                   outChannels, pixels8.data(), static_cast<int>(width) * outChannels) == 0)
        {
            result.Error = "PNG encode failed: " + spec.Source;
            return result;
        }
        result.FileBytes = std::move(encoded);
        result.FileName = spec.Name + ".png";
        return result;
    }

    void ApplyManifestRendererState(const BenchmarkManifest& manifest)
    {
        auto& rendererSettings = Renderer3D::GetRendererSettings();
        const auto& wanted = manifest.RendererSettings;
        if (wanted.Path)
        {
            rendererSettings.Path = *wanted.Path;
        }
        if (wanted.EnableDDGI)
        {
            rendererSettings.EnableDDGI = *wanted.EnableDDGI;
        }
        if (wanted.DepthPrepassEnabled)
        {
            rendererSettings.DepthPrepassEnabled = *wanted.DepthPrepassEnabled;
        }
        if (wanted.OcclusionCullingEnabled)
        {
            rendererSettings.OcclusionCullingEnabled = *wanted.OcclusionCullingEnabled;
        }
        if (wanted.HZBOcclusionCullingEnabled)
        {
            rendererSettings.HZBOcclusionCullingEnabled = *wanted.HZBOcclusionCullingEnabled;
        }

        auto& postProcess = Renderer3D::GetPostProcessSettings();
        postProcess.AutoExposureEnabled = manifest.Exposure == ExposureMode::Auto;
        postProcess.Exposure = manifest.ExposureValue;
        if (wanted.TAAEnabled)
        {
            postProcess.TAAEnabled = *wanted.TAAEnabled;
        }
        // The one setting a capture PINS regardless of the scene: FSR2's
        // temporal locks decay on REAL elapsed time by contract (see
        // RenderPipeline.cpp), so any upscaler makes a mock-clock capture
        // nondeterministic — and a spatial upscale would also break the
        // whole-texture readback's scale assumption.
        postProcess.Upscale = UpscaleMode::Off;

        Renderer3D::ApplyRendererSettings();
        Renderer3D::SetRenderScale(manifest.RenderScale);
    }

    CameraCaptureSet CaptureCameraSet(const BenchmarkManifest& manifest, std::string_view cameraId,
                                      u32 captureFrameIndex, std::string_view backend,
                                      const CaptureContext& context)
    {
        CameraCaptureSet set;
        set.CameraId = std::string(cameraId);
        set.CaptureFrameIndex = captureFrameIndex;
        const std::vector<std::string>* unsupported = manifest.UnsupportedFor(backend);
        for (const auto& spec : manifest.Attachments)
        {
            if (unsupported != nullptr && std::ranges::find(*unsupported, spec.Name) != unsupported->end())
            {
                CapturedAttachment skipped;
                skipped.Spec = spec;
                skipped.SkippedUnsupported = true;
                set.Attachments.push_back(std::move(skipped));
                continue;
            }
            set.Attachments.push_back(CaptureAttachment(spec, context));
        }
        return set;
    }

    std::string QueryCommitShaViaGit()
    {
#ifdef _WIN32
        FILE* pipe = _popen("git rev-parse HEAD 2>nul", "r");
#else
        FILE* pipe = popen("git rev-parse HEAD 2>/dev/null", "r");
#endif
        if (!pipe)
        {
            return "unknown";
        }
        std::string out;
        char buffer[128] = {};
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            out += buffer;
        }
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        {
            out.pop_back();
        }
        const bool looksLikeSha =
            out.size() == 40 && std::ranges::all_of(out,
                                                    [](char c)
                                                    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        return looksLikeSha ? out : "unknown";
    }

    std::string ResolveMachineTag(std::string_view overrideTag)
    {
        std::string tag(overrideTag);
        if (tag.empty())
        {
            if (const auto name = Env::Get("COMPUTERNAME"))
            {
                tag = *name;
            }
            else if (const auto hostname = Env::Get("HOSTNAME"))
            {
                tag = *hostname;
            }
        }
        if (tag.empty())
        {
            tag = "unknown";
        }
        for (char& c : tag)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '-' || c == '_';
            if (!ok)
            {
                c = '_';
            }
        }
        return tag;
    }

    std::vector<PassTimingRecord> SnapshotPassTimings()
    {
        std::vector<PassTimingRecord> records;
        const auto timings = GPUPassTimerPool::GetInstance().GetLastPassTimingsCopy();
        records.reserve(timings.size());
        for (const auto& timing : timings)
        {
            records.push_back({ timing.Name, timing.GpuMs });
        }
        return records;
    }

    RendererCounters SnapshotRendererCounters()
    {
        RendererCounters counters;
        const auto& frame = RendererProfiler::GetInstance().GetLastCompletedFrameData();
        counters.DrawCalls = frame.m_DrawCalls;
        counters.TrianglesRendered = frame.m_TrianglesRendered;
        counters.InstancesRendered = frame.m_InstancesRendered;
        counters.GpuMemoryTotalBytes = static_cast<u64>(RendererMemoryTracker::GetInstance().GetTotalMemoryUsage());
        return counters;
    }

    bool WriteResultDirectory(const BenchmarkManifest& manifest, const std::filesystem::path& manifestSourcePath,
                              const std::filesystem::path& outDir, const std::vector<CameraCaptureSet>& cameraSets,
                              const RunInfo& runInfo, std::string& outError)
    {
        outError.clear();
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        if (ec)
        {
            outError = "cannot create result directory " + outDir.string() + ": " + ec.message();
            return false;
        }

        // Verbatim manifest echo — the result directory names its own recipe.
        std::filesystem::copy_file(manifestSourcePath, outDir / "manifest.yaml",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            outError = "cannot copy manifest into result directory: " + ec.message();
            return false;
        }

        const bool multiCamera = cameraSets.size() > 1;
        for (const auto& set : cameraSets)
        {
            const std::filesystem::path cameraDir = multiCamera ? outDir / set.CameraId : outDir;
            std::filesystem::create_directories(cameraDir, ec);
            if (ec)
            {
                outError = "cannot create camera directory " + cameraDir.string() + ": " + ec.message();
                return false;
            }
            for (const auto& attachment : set.Attachments)
            {
                if (attachment.FileName.empty())
                {
                    continue; // skipped or failed — recorded in result.json instead
                }
                std::ofstream file(cameraDir / attachment.FileName, std::ios::binary | std::ios::trunc);
                if (!file ||
                    !file.write(reinterpret_cast<const char*>(attachment.FileBytes.data()),
                                static_cast<std::streamsize>(attachment.FileBytes.size())))
                {
                    outError = "cannot write " + (cameraDir / attachment.FileName).string();
                    return false;
                }
            }
        }

        nlohmann::json json;
        json["resultSchemaVersion"] = 1;
        json["manifest"] = { { "id", manifest.Id },
                             { "version", manifest.ManifestVersion },
                             { "sourceHashFnv1a64", manifest.SourceHash },
                             { "scene", manifest.ScenePath },
                             { "product", manifest.Product == ManifestProduct::Golden       ? "golden"
                                          : manifest.Product == ManifestProduct::Diagnostic ? "diagnostic"
                                                                                            : "hero" } };
        json["provenance"] = { { "backend", runInfo.Backend },
                               { "gpuVendor", runInfo.GpuVendor },
                               { "gpuRenderer", runInfo.GpuRenderer },
                               { "commitSha", runInfo.CommitSha },
                               { "machineTag", runInfo.MachineTag },
                               { "host", runInfo.Host } };
        json["output"] = { { "width", manifest.Width },
                           { "height", manifest.Height },
                           { "renderScale", manifest.RenderScale } };
        json["determinism"] = { { "seed", manifest.Seed },
                                { "startTimeSeconds", manifest.StartTimeSeconds },
                                { "fixedDtSeconds", manifest.FixedDtSeconds },
                                { "totalFramesRendered", runInfo.TotalFramesRendered },
                                { "finalMockTimeSeconds", runInfo.FinalMockTimeSeconds },
                                { "repeatRmseTolerance", manifest.RepeatRmseTolerance } };
        json["exposure"] = { { "mode", manifest.Exposure == ExposureMode::Manual ? "manual" : "auto" },
                             { "exposure", manifest.ExposureValue } };
        json["warmup"] = { { "frames", manifest.WarmupFrames } };
        for (const auto& [feature, frames] : manifest.WarmupPerFeature)
        {
            json["warmup"]["perFeature"][feature] = frames;
        }

        json["cameras"] = nlohmann::json::array();
        for (const auto& set : cameraSets)
        {
            nlohmann::json cameraJson;
            cameraJson["id"] = set.CameraId;
            cameraJson["captureFrameIndex"] = set.CaptureFrameIndex;
            cameraJson["attachments"] = nlohmann::json::array();
            for (const auto& attachment : set.Attachments)
            {
                nlohmann::json a;
                a["name"] = attachment.Spec.Name;
                a["source"] = attachment.Spec.Source;
                if (attachment.SkippedUnsupported)
                {
                    a["skipped"] = !attachment.SkipReason.empty()
                                       ? attachment.SkipReason
                                       : "declared unsupported for backend " + runInfo.Backend;
                }
                else if (!attachment.Error.empty())
                {
                    a["error"] = attachment.Error;
                }
                else
                {
                    a["file"] = (cameraSets.size() > 1 ? set.CameraId + "/" : "") + attachment.FileName;
                    a["width"] = attachment.Width;
                    a["height"] = attachment.Height;
                    a["sourceFormat"] = attachment.FormatName;
                    a["isDepth"] = attachment.IsDepth;
                    a["normalized"] = attachment.Normalized;
                    a["minValue"] = attachment.MinValue;
                    a["maxValue"] = attachment.MaxValue;
                }
                cameraJson["attachments"].push_back(std::move(a));
            }
            json["cameras"].push_back(std::move(cameraJson));
        }

        json["passTimingsMs"] = nlohmann::json::array();
        for (const auto& timing : runInfo.PassTimings)
        {
            json["passTimingsMs"].push_back({ { "pass", timing.Name }, { "gpuMs", timing.GpuMs } });
        }
        json["rendererCounters"] = { { "drawCalls", runInfo.Counters.DrawCalls },
                                     { "trianglesRendered", runInfo.Counters.TrianglesRendered },
                                     { "instancesRendered", runInfo.Counters.InstancesRendered },
                                     { "gpuMemoryTotalBytes", runInfo.Counters.GpuMemoryTotalBytes } };

        std::ofstream resultFile(outDir / "result.json", std::ios::binary | std::ios::trunc);
        const std::string serialized = json.dump(2);
        if (!resultFile || !resultFile.write(serialized.data(), static_cast<std::streamsize>(serialized.size())))
        {
            outError = "cannot write " + (outDir / "result.json").string();
            return false;
        }
        return true;
    }
} // namespace OloEngine::Benchmark
