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
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Support/RendererSupport.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
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
#include <numbers>
#include <set>

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
        RHI::ResourceHandle ResolveTargetHandle(std::string_view name, bool& outDepthFromFramebuffer)
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

        void AppendEncodedBytes(void* context, void* data, int size)
        {
            auto* out = static_cast<TArray<u8>*>(context);
            const auto* bytes = static_cast<const u8*>(data);
            out->Append(bytes, size);
        }

        // Neither PNG (comp=2 is grey+alpha) nor Radiance .hdr has a meaningful
        // 2-component layout — widen RG to RGB with a zeroed blue lane, once,
        // for both encode paths.
        template<typename T>
        void WidenRGToRGB(TArray<T>& data, sizet texels)
        {
            TArray<T> widened;
            widened.SetNum(static_cast<i32>(texels * 3u));
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
        const RHI::ResourceHandle handle = ResolveTargetHandle(spec.Source.ToView(), depthFromFramebuffer);
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
        const auto* resource = graph->FindRegisteredResource(spec.Source.ToView());
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

        // Native array indexing is signed; reject unrepresentable readbacks,
        // including the possible two-channel to RGB expansion, before allocation.
        if (static_cast<u64>(width) * height >
            static_cast<u64>(std::numeric_limits<i32>::max()) / static_cast<u64>(std::max(3, readChannels)))
        {
            result.Error = "capture dimensions exceed native array capacity";
            return result;
        }
        sizet valueCount = static_cast<sizet>(width) * height * static_cast<sizet>(readChannels);
        TArray<f32> values;
        values.SetNum(static_cast<i32>(valueCount));
        if (!RenderCommand::ReadTextureSubImage(handle, 0, 0, 0, 0, width, height, 1u, destFormat,
                                                values.Num() * sizeof(f32), values.GetData()))
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
            TArray<f32> derived;
            derived.SetNum(static_cast<i32>(texels));
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
        const auto flipFloatRows = [&](TArray<f32>& data, i32 comps)
        {
            if (!flipRows)
            {
                return;
            }
            const sizet rowValues = static_cast<sizet>(width) * comps;
            TArray<f32> scratch;
            scratch.SetNum(static_cast<i32>(rowValues));
            for (sizet y = 0; y < static_cast<sizet>(height) / 2u; ++y)
            {
                f32* top = data.GetData() + y * rowValues;
                f32* bottom = data.GetData() + (static_cast<sizet>(height) - 1 - y) * rowValues;
                std::memcpy(scratch.GetData(), top, rowValues * sizeof(f32));
                std::memcpy(top, bottom, rowValues * sizeof(f32));
                std::memcpy(bottom, scratch.GetData(), rowValues * sizeof(f32));
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
            TArray<u8> encoded;
            if (stbi_write_hdr_to_func(AppendEncodedBytes, &encoded, static_cast<int>(width), static_cast<int>(height),
                                       outChannels, values.GetData()) == 0)
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

        TArray<u8> pixels8;
        pixels8.SetNum(static_cast<i32>(valueCount));
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

        TArray<u8> encoded;
        if (stbi_write_png_to_func(AppendEncodedBytes, &encoded, static_cast<int>(width), static_cast<int>(height),
                                   outChannels, pixels8.GetData(), static_cast<int>(width) * outChannels) == 0)
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
        if (wanted.MSAASampleCount)
        {
            rendererSettings.Deferred.MSAASampleCount = *wanted.MSAASampleCount;
        }

        auto& postProcess = Renderer3D::GetPostProcessSettings();
        postProcess.AutoExposureEnabled = manifest.Exposure == ExposureMode::Auto;
        postProcess.Exposure = manifest.ExposureValue;
        if (wanted.TAAEnabled)
        {
            postProcess.TAAEnabled = *wanted.TAAEnabled;
            // A pinned TAA axis must mean what it says (#1429): a scene holding a
            // stochastic groom requests engine TAA on its own, so a "TAA off" arm
            // would otherwise measure TAA anyway. Pinned off, it measures the
            // no-resolve frame (bald coats included); pinned on, nothing changes.
            rendererSettings.HonourSceneTemporalResolveRequests = *wanted.TAAEnabled;
        }
        if (wanted.RayTracedShadowsEnabled)
        {
            auto settings = Renderer3D::GetShadowMap().GetSettings();
            settings.Technique = *wanted.RayTracedShadowsEnabled ? ShadowTechnique::RayTraced : ShadowTechnique::ShadowMap;
            Renderer3D::GetShadowMap().SetSettings(settings);
        }
        if (wanted.GpuPathTracerEnabled)
        {
            postProcess.GpuPathTracer.Enabled = *wanted.GpuPathTracerEnabled;
        }
        if (wanted.GpuPathTracerSamplesPerFrame)
        {
            postProcess.GpuPathTracer.SamplesPerFrame = *wanted.GpuPathTracerSamplesPerFrame;
        }
        postProcess.Upscale = wanted.Upscale.value_or(UpscaleMode::Off);
        postProcess.Technique = wanted.UpscaleTechnique.value_or(UpscalerTechnique::Spatial);

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
        const TArray<FString>* unsupported = manifest.UnsupportedFor(backend);
        for (const auto& spec : manifest.Attachments)
        {
            if (unsupported != nullptr && std::ranges::find(*unsupported, spec.Name) != unsupported->end())
            {
                CapturedAttachment skipped;
                skipped.Spec = spec;
                skipped.SkippedUnsupported = true;
                set.Attachments.Add(std::move(skipped));
                continue;
            }
            set.Attachments.Add(CaptureAttachment(spec, context));
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

    MeasuredFrame SnapshotMeasuredFrame(std::string_view cameraId, u32 index, f64 renderCallMs)
    {
        MeasuredFrame sample;
        sample.CameraId = cameraId;
        sample.Index = index;
        sample.RenderCallMs = renderCallMs;
        const auto& frame = RendererProfiler::GetInstance().GetLastCompletedFrameData();
        sample.CpuMs = frame.m_CPUTime;
        sample.FenceWaitMs = frame.m_FenceWaitTime;
        sample.PresentWaitMs = frame.m_PresentWaitTime;
        sample.DrawCalls = frame.m_DrawCalls;
        const auto gpu = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
        sample.GpuFrameId = gpu.FrameNumber;
        sample.Gpu = gpu.Frame;
        if (gpu.IsStale())
        {
            sample.Gpu = GpuTimingSample::Absent(GpuTimingStatus::Unavailable);
        }
        sample.TrackedRendererBytes = static_cast<u64>(RendererMemoryTracker::GetInstance().GetTotalMemoryUsage());
        return sample;
    }

    MeasuredFrame SnapshotEditorMeasuredFrame(std::string_view cameraId, u32 index)
    {
        const auto& frame = RendererProfiler::GetInstance().GetLastCompletedFrameData();
        return SnapshotMeasuredFrame(cameraId, index, frame.m_FrameTime);
    }

    AppliedConfiguration SnapshotAppliedConfiguration()
    {
        AppliedConfiguration applied;
        applied.Path = Renderer3D::GetRendererSettings().Path;
        applied.MSAASampleCount = Renderer3D::GetRendererSettings().Deferred.MSAASampleCount;
        applied.Upscale = Renderer3D::GetPostProcessSettings().Upscale;
        applied.Technique = Renderer3D::GetPostProcessSettings().Technique;
        applied.RayTracedShadowsRequested = Renderer3D::GetShadowMap().GetSettings().Technique == ShadowTechnique::RayTraced;
        return applied;
    }

    bool ApplyEntityMotion(Scene& scene, const BenchmarkManifest& manifest, u32 frameIndex,
                           std::string& outError)
    {
        const f32 elapsed = static_cast<f32>(frameIndex) * manifest.FixedDtSeconds;
        for (const auto& motion : manifest.EntityMotions)
        {
            Entity entity = scene.FindEntityByName(motion.Tag.ToView());
            if (!entity || !entity.HasComponent<TransformComponent>())
            {
                outError = "EntityMotion tag missing or without transform: " + motion.Tag.ToStdString();
                return false;
            }
            const f32 phase = motion.PhaseRadians + elapsed * (2.0f * std::numbers::pi_v<f32>)*motion.FrequencyHz;
            entity.GetComponent<TransformComponent>().Translation = motion.Origin + motion.Amplitude * std::sin(phase);
        }
        return true;
    }

    bool ValidatePresetAdmission(const BenchmarkManifest& manifest, std::string_view backend,
                                 std::string& outError)
    {
        const auto entry = manifest.PresetIdsByBackend.find(std::string(backend));
        if (entry == manifest.PresetIdsByBackend.end())
            return true; // legacy capture manifests have no production preset
        for (const auto& preset : RendererSupport::Presets)
        {
            if (preset.Name != entry->second.ToView())
                continue;
            const auto requestedBackend = backend == "vulkan" ? RendererSupport::Backend::Vulkan
                                                              : RendererSupport::Backend::OpenGL;
            if (preset.Api != requestedBackend)
            {
                outError = "manifest backend disagrees with preset " + entry->second.ToStdString();
                return false;
            }
            if (!manifest.RendererSettings.Path || *manifest.RendererSettings.Path != preset.Path)
            {
                outError = "manifest path disagrees with preset " + entry->second.ToStdString();
                return false;
            }
            if (!manifest.RendererSettings.RayTracedShadowsEnabled)
            {
                outError = "manifest ray-traced shadow setting missing for preset " + entry->second.ToStdString();
                return false;
            }
            const bool hybrid = preset.LightingTechnique != RendererSupport::Technique::Raster;
            if (manifest.RendererSettings.RayTracedShadowsEnabled.value_or(false) != hybrid)
            {
                outError = "manifest ray-traced shadow setting disagrees with preset " + entry->second.ToStdString();
                return false;
            }
            RendererSupport::Capabilities capabilities;
            capabilities.RayQueries = RenderCommand::SupportsRayTracing();
            capabilities.TemporalUpscaler = backend == "opengl";
            capabilities.MaxSamples = std::max(1u, Renderer3D::GetMaxMSAASamples());
            const auto decision = RendererSupport::EvaluatePreset(preset.Id, capabilities);
            if (decision.Status == RendererSupport::Outcome::Unsupported)
            {
                outError = "preset " + entry->second.ToStdString() + " unsupported: " +
                           std::string(RendererSupport::ToString(decision.Why));
                return false;
            }
            return true;
        }
        outError = "unknown renderer-support preset " + entry->second.ToStdString();
        return false;
    }

    TArray<PassTimingRecord> SnapshotPassTimings()
    {
        TArray<PassTimingRecord> records;
        const GPUPassTimerPool::FrameTimings frame = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
        records.Reserve(frame.Passes.Num());
        for (const auto& timing : frame.Passes)
        {
            records.Add(PassTimingRecord{ timing.Name, timing.Sample, timing.IsSubPass, timing.ParentName });
        }
        return records;
    }

    TimingValidity SnapshotTimingValidity()
    {
        const GPUPassTimerPool::FrameTimings frame = GPUPassTimerPool::GetInstance().GetLastFrameTimings();
        TimingValidity validity;
        validity.MeasurementFrameId = frame.FrameNumber;
        validity.CurrentFrameId = frame.CurrentFrameNumber;
        validity.AgeFrames = frame.AgeFrames;
        validity.DroppedSlots = frame.DroppedSlots;
        validity.UnstampedFrames = frame.UnstampedFrames;
        validity.Stale = frame.IsStale();
        validity.FrameStatus = frame.Frame.Status;
        return validity;
    }

    ResolutionRecord SnapshotResolution()
    {
        ResolutionRecord record;
        // The live graph is the only thing that knows the render scale actually
        // in force; the manifest carries the request, which is not the same
        // number once anything has touched the scale.
        if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph())
        {
            record.RenderWidth = graph->GetRenderWidth();
            record.RenderHeight = graph->GetRenderHeight();
            record.DisplayWidth = graph->GetPhysicalWidth();
            record.DisplayHeight = graph->GetPhysicalHeight();
            record.RenderScale = graph->GetRenderScale();
            record.Measured = true;
        }
        return record;
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
                              const std::filesystem::path& outDir, std::span<const CameraCaptureSet> cameraSets,
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
            const std::filesystem::path cameraDir = multiCamera ? outDir / set.CameraId.ToStdString() : outDir;
            std::filesystem::create_directories(cameraDir, ec);
            if (ec)
            {
                outError = "cannot create camera directory " + cameraDir.string() + ": " + ec.message();
                return false;
            }
            for (const auto& attachment : set.Attachments)
            {
                if (attachment.FileName.IsEmpty())
                {
                    continue; // skipped or failed — recorded in result.json instead
                }
                std::ofstream file(cameraDir / attachment.FileName.ToStdString(), std::ios::binary | std::ios::trunc);
                if (!file ||
                    !file.write(reinterpret_cast<const char*>(attachment.FileBytes.GetData()),
                                static_cast<std::streamsize>(attachment.FileBytes.Num())))
                {
                    outError = "cannot write " + (cameraDir / attachment.FileName.ToStdString()).string();
                    return false;
                }
            }
        }

        nlohmann::json json;
        // v2 (#1337): pass timings gained `gpuMs: null` + `status`, the export
        // gained `timingValidity` and actual render/display dimensions, and
        // `units` names what every number is in. See
        // docs/guides/renderer-benchmarks.md for the compatibility story — a v1
        // reader sees the same keys it always did, with `gpuMs` sometimes null.
        json["resultSchemaVersion"] = 2;
        // Spelled out rather than implied. A persisted export is read months
        // later by something that was not there when it was written.
        json["units"] = { { "gpuMs", "milliseconds" },
                          { "cpuMs", "milliseconds" },
                          { "bytes", "bytes" },
                          { "dimensions", "pixels" },
                          { "frameIds", "GPUPassTimerPool frame counter, monotonic from renderer init" } };
        json["manifest"] = { { "id", manifest.Id.ToStdString() },
                             { "version", manifest.ManifestVersion },
                             { "sourceHashFnv1a64", manifest.SourceHash },
                             { "scene", manifest.ScenePath.ToStdString() },
                             { "product", manifest.Product == ManifestProduct::Golden       ? "golden"
                                          : manifest.Product == ManifestProduct::Diagnostic ? "diagnostic"
                                                                                            : "hero" } };
        json["provenance"] = { { "backend", runInfo.Backend.ToStdString() },
                               { "gpuVendor", runInfo.GpuVendor.ToStdString() },
                               { "gpuRenderer", runInfo.GpuRenderer.ToStdString() },
                               { "commitSha", runInfo.CommitSha.ToStdString() },
                               { "machineTag", runInfo.MachineTag.ToStdString() },
                               { "host", runInfo.Host.ToStdString() } };
        // The manifest's numbers are the REQUEST; `actual` is what the render
        // graph did with it (#1337 criterion 4). They differ whenever anything
        // has touched the render scale, and a record carrying only the request
        // cannot be compared against one taken at a different scale.
        json["output"] = { { "width", manifest.Width },
                           { "height", manifest.Height },
                           { "renderScale", manifest.RenderScale },
                           { "requested", { { "width", manifest.Width }, { "height", manifest.Height }, { "renderScale", manifest.RenderScale } } } };
        if (runInfo.Resolution.Measured)
        {
            json["output"]["actual"] = { { "renderWidth", runInfo.Resolution.RenderWidth },
                                         { "renderHeight", runInfo.Resolution.RenderHeight },
                                         { "displayWidth", runInfo.Resolution.DisplayWidth },
                                         { "displayHeight", runInfo.Resolution.DisplayHeight },
                                         { "renderScale", runInfo.Resolution.RenderScale } };
        }
        else
        {
            // No live graph to ask. Null rather than an echo of the request,
            // which would assert a measurement that was never taken.
            json["output"]["actual"] = nullptr;
        }
        json["determinism"] = { { "seed", manifest.Seed },
                                { "startTimeSeconds", manifest.StartTimeSeconds },
                                { "fixedDtSeconds", manifest.FixedDtSeconds },
                                { "totalFramesRendered", runInfo.TotalFramesRendered },
                                { "warmupTimedOut", runInfo.WarmupTimedOut },
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
            cameraJson["id"] = set.CameraId.ToStdString();
            cameraJson["captureFrameIndex"] = set.CaptureFrameIndex;
            // The pose the CAPTURED frame was actually rendered from. For a
            // still camera that is the declared pose; for a moving one it is
            // the integrated pose at the last warm-up frame, which is the only
            // value that lets anyone reconstruct the shot (issue #1239).
            if (const auto spec = std::ranges::find_if(manifest.Cameras, [&set](const ManifestCamera& c)
                                                       { return c.Id == set.CameraId; });
                spec != manifest.Cameras.end())
            {
                const u32 warmFrames = spec->WarmupFrames.value_or(manifest.WarmupFrames);
                const u32 cameraFrames = warmFrames + manifest.MeasurementFrames;
                const auto pose = CameraPoseAtFrame(*spec, cameraFrames > 0u ? cameraFrames - 1u : 0u,
                                                    manifest.FixedDtSeconds);
                cameraJson["warmupFrames"] = warmFrames;
                if (runInfo.WarmupTimedOut)
                {
                    // The pose below is computed from the DECLARED warm-up
                    // count. If the host could not confirm those frames
                    // rendered, it may name a frame that never happened, so the
                    // claim is qualified rather than made silently.
                    cameraJson["capturedPoseUnreliable"] =
                        "warm-up did not complete within the host's deadline — this pose is the "
                        "scheduled pose for the declared frame count, not a confirmed one";
                }
                cameraJson["capturedPose"] = { { "position", { pose.Position.x, pose.Position.y, pose.Position.z } },
                                               { "yawDegrees", pose.YawDegrees },
                                               { "pitchDegrees", pose.PitchDegrees },
                                               { "fovDegrees", spec->FovDegrees },
                                               { "near", spec->NearClip },
                                               { "far", spec->FarClip } };
                if (spec->Motion)
                {
                    cameraJson["motion"] = {
                        { "velocityPerSecond",
                          { spec->Motion->VelocityPerSecond.x, spec->Motion->VelocityPerSecond.y,
                            spec->Motion->VelocityPerSecond.z } },
                        { "yawRateDegreesPerSecond", spec->Motion->YawRateDegreesPerSecond },
                        { "pitchRateDegreesPerSecond", spec->Motion->PitchRateDegreesPerSecond }
                    };
                }
            }
            cameraJson["attachments"] = nlohmann::json::array();
            for (const auto& attachment : set.Attachments)
            {
                nlohmann::json a;
                a["name"] = attachment.Spec.Name.ToStdString();
                a["source"] = attachment.Spec.Source.ToStdString();
                if (attachment.SkippedUnsupported)
                {
                    a["skipped"] = !attachment.SkipReason.IsEmpty()
                                       ? attachment.SkipReason.ToStdString()
                                       : "declared unsupported for backend " + runInfo.Backend.ToStdString();
                }
                else if (!attachment.Error.IsEmpty())
                {
                    a["error"] = attachment.Error.ToStdString();
                }
                else
                {
                    a["file"] = (cameraSets.size() > 1 ? set.CameraId.ToStdString() + "/" : "") + attachment.FileName.ToStdString();
                    a["width"] = attachment.Width;
                    a["height"] = attachment.Height;
                    a["sourceFormat"] = attachment.FormatName.ToStdString();
                    a["isDepth"] = attachment.IsDepth;
                    a["normalized"] = attachment.Normalized;
                    a["minValue"] = attachment.MinValue;
                    a["maxValue"] = attachment.MaxValue;
                }
                cameraJson["attachments"].push_back(std::move(a));
            }
            json["cameras"].push_back(std::move(cameraJson));
        }

        // Asset provenance, echoed verbatim into the result (issue #1239). A
        // result directory that cannot say where its content came from is not
        // self-describing, and the manifest echo alone is easy to lose track
        // of once captures are copied around.
        json["assets"] = nlohmann::json::array();
        for (const auto& asset : manifest.Assets)
        {
            nlohmann::json a;
            a["path"] = asset.Path.ToStdString();
            a["origin"] = asset.Origin.ToStdString();
            a["license"] = asset.License.ToStdString();
            if (asset.Redistribution)
            {
                a["redistribution"] = *asset.Redistribution == AssetRedistribution::Committed ? "committed"
                                      : *asset.Redistribution == AssetRedistribution::FetchRequired
                                          ? "fetch-required"
                                          : "local-only";
            }
            if (asset.LicenseVerified)
            {
                a["licenseVerified"] = *asset.LicenseVerified == LicenseVerification::InRepoFile ? "in-repo-file"
                                       : *asset.LicenseVerified == LicenseVerification::UpstreamDeclared
                                           ? "upstream-declared"
                                           : "unverified";
            }
            if (asset.Units)
            {
                a["units"] = *asset.Units == AssetUnits::Metres        ? "metres"
                             : *asset.Units == AssetUnits::Centimetres ? "centimetres"
                                                                       : "unitless";
            }
            if (asset.UpAxis)
            {
                a["upAxis"] = *asset.UpAxis == AssetUpAxis::YUp ? "+Y" : *asset.UpAxis == AssetUpAxis::ZUp ? "+Z"
                                                                                                           : "n/a";
            }
            if (asset.ColorSpace)
            {
                a["colorSpace"] = *asset.ColorSpace == AssetColorSpace::Srgb     ? "srgb"
                                  : *asset.ColorSpace == AssetColorSpace::Linear ? "linear"
                                                                                 : "n/a";
            }
            if (!asset.Version.IsEmpty())
            {
                a["version"] = asset.Version.ToStdString();
            }
            if (!asset.Sha256.IsEmpty())
            {
                a["sha256"] = asset.Sha256.ToStdString();
            }
            if (!asset.Acquisition.IsEmpty())
            {
                a["acquisition"] = asset.Acquisition.ToStdString();
            }
            json["assets"].push_back(std::move(a));
        }

        // Each entry is a number-or-null plus the status that says which
        // (#1337). `gpuMs: 0` used to mean both "free" and "never measured",
        // and in a file that outlives the run there is no way to ask afterwards.
        json["passTimingsMs"] = nlohmann::json::array();
        for (const auto& timing : runInfo.PassTimings)
        {
            nlohmann::json entry{ { "pass", timing.Name.ToStdString() },
                                  { "status", std::string(ToString(timing.Sample.Status)) },
                                  { "isSubPass", timing.IsSubPass } };
            if (timing.Sample.IsValid())
            {
                entry["gpuMs"] = timing.Sample.GpuMs;
            }
            else
            {
                entry["gpuMs"] = nullptr;
            }
            if (timing.IsSubPass)
            {
                entry["parent"] = timing.ParentName.ToStdString();
            }
            json["passTimingsMs"].push_back(std::move(entry));
        }
        // Which frame the timings above describe, how old they were when taken,
        // and how many frames the ring lost outright. Without this the export
        // records numbers with no way to judge them later.
        json["timingValidity"] = { { "measurementFrameId", runInfo.Timing.MeasurementFrameId },
                                   { "currentFrameId", runInfo.Timing.CurrentFrameId },
                                   { "ageFrames", runInfo.Timing.AgeFrames },
                                   { "droppedSlots", runInfo.Timing.DroppedSlots },
                                   { "unstampedFrames", runInfo.Timing.UnstampedFrames },
                                   { "stale", runInfo.Timing.Stale },
                                   { "frameStatus", std::string(ToString(runInfo.Timing.FrameStatus)) } };
        json["rendererCounters"] = { { "drawCalls", runInfo.Counters.DrawCalls },
                                     { "trianglesRendered", runInfo.Counters.TrianglesRendered },
                                     { "instancesRendered", runInfo.Counters.InstancesRendered },
                                     { "gpuMemoryTotalBytes", runInfo.Counters.GpuMemoryTotalBytes } };
        const auto pathName = [](RenderingPath path) -> const char*
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        };
        json["configuration"] = {
            { "requested", { { "path", manifest.RendererSettings.Path ? pathName(*manifest.RendererSettings.Path) : "unfixed" }, { "msaaSamples", manifest.RendererSettings.MSAASampleCount.value_or(1u) }, { "upscaleMode", static_cast<i32>(manifest.RendererSettings.Upscale.value_or(UpscaleMode::Off)) }, { "upscaleTechnique", static_cast<i32>(manifest.RendererSettings.UpscaleTechnique.value_or(UpscalerTechnique::Spatial)) }, { "rayTracedShadows", manifest.RendererSettings.RayTracedShadowsEnabled.value_or(false) } } },
            { "selectedSettings", { { "path", pathName(runInfo.Configuration.Path) }, { "msaaSamples", runInfo.Configuration.MSAASampleCount }, { "upscaleMode", static_cast<i32>(runInfo.Configuration.Upscale) }, { "upscaleTechnique", static_cast<i32>(runInfo.Configuration.Technique) }, { "rayTracedShadowsRequested", runInfo.Configuration.RayTracedShadowsRequested } } },
            { "production", "unknown without pass and counter evidence" },
            { "consumption", "unknown without downstream evidence" }
        };

        if (!runInfo.Measurement.Frames.IsEmpty())
        {
            // Preserve every frame in a separate raw file; a percentile alone
            // cannot show transient stalls, warm/cold drift, or an invalid GPU
            // timestamp being accidentally interpreted as a fast frame.
            std::ofstream raw(outDir / "measurement.csv", std::ios::binary | std::ios::trunc);
            raw << "camera,index,renderCallMs,cpuMs,fenceWaitMs,presentWaitMs,gpuFrameId,gpuMs,gpuStatus,trackedRendererBytes,drawCalls\n";
            std::vector<f64> wall;
            wall.reserve(runInfo.Measurement.Frames.Num());
            std::map<std::string, std::vector<f64>> byCamera;
            std::set<u64> validGpuFrames;
            u32 missed = 0;
            u64 peakTrackedBytes = 0;
            for (const auto& frame : runInfo.Measurement.Frames)
            {
                wall.push_back(frame.RenderCallMs);
                byCamera[frame.CameraId.ToStdString()].push_back(frame.RenderCallMs);
                missed += frame.RenderCallMs > runInfo.Measurement.DeadlineMs ? 1u : 0u;
                peakTrackedBytes = std::max(peakTrackedBytes, frame.TrackedRendererBytes);
                raw << frame.CameraId.ToView() << ',' << frame.Index << ',' << frame.RenderCallMs << ','
                    << frame.CpuMs << ',' << frame.FenceWaitMs << ',' << frame.PresentWaitMs << ','
                    << frame.GpuFrameId << ',';
                const bool uniqueGpu = frame.Gpu.IsValid() && frame.GpuFrameId != 0 &&
                                       validGpuFrames.insert(frame.GpuFrameId).second;
                if (uniqueGpu)
                    raw << frame.Gpu.GpuMs;
                raw << ',' << (frame.Gpu.IsValid() && !uniqueGpu ? "duplicate" : ToString(frame.Gpu.Status))
                    << ',' << frame.TrackedRendererBytes << ',' << frame.DrawCalls << '\n';
            }
            if (!raw)
            {
                outError = "cannot write " + (outDir / "measurement.csv").string();
                return false;
            }
            std::ranges::sort(wall);
            const auto percentile = [&wall](f64 q) -> f64
            {
                const auto rank = static_cast<sizet>(std::ceil(q * static_cast<f64>(wall.size())));
                return wall[std::clamp<sizet>(rank, 1, wall.size()) - 1];
            };
            json["measurement"] = { { "rawFile", "measurement.csv" },
                                    { "metric", runInfo.Host == "editor-mcp"
                                                    ? "completed editor frame interval; sampling marshals may perturb it"
                                                    : "wall-clock Scene::OnUpdateEditor call, excluding attachment readback" },
                                    { "sampleCount", wall.size() },
                                    { "deadlineMs", runInfo.Measurement.DeadlineMs },
                                    { "deadlineMisses", missed },
                                    { "p50Ms", percentile(0.50) },
                                    { "p95Ms", percentile(0.95) },
                                    { "p99Ms", percentile(0.99) },
                                    { "maxMs", wall.back() },
                                    { "distinctValidGpuFrames", validGpuFrames.size() },
                                    { "peakTrackedRendererBytes", peakTrackedBytes },
                                    { "liveTrackedRendererBytes", runInfo.Counters.GpuMemoryTotalBytes },
                                    { "trackedRendererBytesAfterSceneRelease", runInfo.TrackedRendererBytesAfterSceneRelease
                                                                                   ? nlohmann::json(*runInfo.TrackedRendererBytesAfterSceneRelease)
                                                                                   : nlohmann::json(nullptr) },
                                    { "memoryScope", "renderer tracker total mixes CPU and GPU allocations; post-scene-release value includes asset and renderer caches; retained pools, histories and AS bytes not isolated" } };
            json["measurement"]["scenarios"] = nlohmann::json::array();
            for (auto& [camera, values] : byCamera)
            {
                std::ranges::sort(values);
                const auto at = [&values](f64 q) -> f64
                {
                    const auto rank = static_cast<sizet>(std::ceil(q * static_cast<f64>(values.size())));
                    return values[std::clamp<sizet>(rank, 1, values.size()) - 1];
                };
                const auto misses = std::ranges::count_if(values, [&](f64 ms)
                                                          { return ms > runInfo.Measurement.DeadlineMs; });
                json["measurement"]["scenarios"].push_back({ { "camera", camera },
                                                             { "sampleCount", values.size() },
                                                             { "deadlineMisses", misses },
                                                             { "p50Ms", at(0.50) },
                                                             { "p95Ms", at(0.95) },
                                                             { "p99Ms", at(0.99) },
                                                             { "maxMs", values.back() } });
            }
        }

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
