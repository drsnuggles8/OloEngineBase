#pragma once

#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkCapture.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderGraph.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    inline void ExpectAttachmentSnapshotSamplingMatchesSource(std::string_view source, std::string_view snapshot)
    {
        auto graph = RenderGraphDebugRuntime::GetActiveGraph();
        ASSERT_TRUE(graph);
        const u32 sourceTexture = graph->ResolveTexture(graph->GetTextureHandle(source));
        const u32 snapshotTexture = graph->ResolveTexture(graph->GetTextureHandle(snapshot));
        ASSERT_NE(sourceTexture, 0u);
        ASSERT_NE(snapshotTexture, 0u);
        // These fixtures are GL-only. Compare the production objects' sampling
        // state: equal texels alone do not preserve a screen-edge blur when a
        // framebuffer's clamp-to-edge source becomes a repeating snapshot.
        for (const GLenum parameter : { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T })
        {
            GLint sourceValue = 0;
            GLint snapshotValue = 0;
            glGetTextureParameteriv(sourceTexture, parameter, &sourceValue);
            glGetTextureParameteriv(snapshotTexture, parameter, &snapshotValue);
            EXPECT_EQ(snapshotValue, sourceValue) << snapshot << " sampling parameter " << parameter;
        }
    }

    // The forward targets are single-sample; Deferred owns the user MSAA
    // setting. Bilinear reconstruction uses a dynamic render scale with the
    // FSR preset off. FSR2 requests explicitly check the production fallback
    // when a resolved MSAA input (or unavailable backend) prevents it running.
    template<typename CaptureFn>
    void ForEachAttachmentFeedbackCell(RenderingPath path, std::string_view pathName, CaptureFn capture)
    {
        struct RestoreScale
        {
            f32 Saved = Renderer3D::GetRenderScale();
            ~RestoreScale()
            {
                Renderer3D::SetRenderScale(Saved);
            }
        } const restore;
        const u32 supportedSamples = std::clamp(Renderer3D::GetMaxMSAASamples(), 1u, 4u);
        for (const u32 samples : { 1u, supportedSamples })
        {
            for (const std::string_view arm : { "Native", "Bilinear", "FSR2" })
            {
                const u32 effectiveSamples = path == RenderingPath::Deferred ? samples : 1u;
                const std::string sampleName = samples != effectiveSamples ? "MSAARequest" + std::to_string(samples) + "xActual1x" : std::to_string(samples) + "x";
                const std::string cell = std::string(pathName) + "_" + sampleName + "_" + std::string(arm) +
                                         (arm == "Bilinear" && path == RenderingPath::Deferred ? "_Rejected1537" : "");
                SCOPED_TRACE(cell);
                auto& settings = Renderer3D::GetRendererSettings();
                settings.Path = path;
                settings.Deferred.MSAASampleCount = samples;
                auto& post = Renderer3D::GetPostProcessSettings();
                post.Upscale = arm == "FSR2" ? UpscaleMode::Quality : UpscaleMode::Off;
                post.Technique = arm == "FSR2" ? UpscalerTechnique::Temporal : UpscalerTechnique::Spatial;
                Renderer3D::SetRenderScale(arm == "Bilinear" ? 0.75f : 1.0f);
                Renderer3D::ApplyRendererSettings();
                capture(cell, arm == "FSR2" ? 16u : 2u);
                if (::testing::Test::HasFatalFailure())
                    return;
                if (arm == "Bilinear" && path == RenderingPath::Deferred)
                {
                    Benchmark::ManifestAttachment spec;
                    spec.Name = "Beauty";
                    spec.Source = "UIComposite";
                    spec.Format = Benchmark::AttachmentFormat::Png;
                    const auto rejected = Benchmark::CaptureAttachment(spec);
                    EXPECT_NE(rejected.Error.ToStdString().find("#1537"), std::string::npos)
                        << "Deferred dynamic scaling must report its current support rejection";
                }
                const auto& resolution = Renderer3D::GetUpscaleResolution();
                EXPECT_TRUE(resolution.Latched);
                EXPECT_EQ(resolution.Path, path);
                EXPECT_EQ(resolution.SceneSampleCount, effectiveSamples);
                if (arm == "FSR2")
                {
                    if (effectiveSamples > 1u)
                    {
                        EXPECT_FALSE(Renderer3D::IsTemporalUpscaleActive());
                        EXPECT_EQ(resolution.Result.Fallback, TemporalUpscalePolicy::TemporalFallback::MSAAResolved);
                    }
                    else if (resolution.UpscalerStatus == TemporalUpscalerStatus::Available)
                    {
                        EXPECT_TRUE(Renderer3D::IsTemporalUpscaleActive());
                    }
                    else
                    {
                        EXPECT_FALSE(Renderer3D::IsTemporalUpscaleActive());
                        EXPECT_EQ(resolution.Result.Resolved, TemporalUpscalePolicy::ResolvedUpscaler::Spatial);
                        EXPECT_EQ(resolution.Result.Fallback, TemporalUpscalePolicy::TemporalFallback::UpscalerUnavailable);
                    }
                }
            }
        }
    }

    inline void ExpectAttachmentFeedbackGraphClean(std::string_view passName)
    {
        auto graph = RenderGraphDebugRuntime::GetActiveGraph();
        ASSERT_TRUE(graph);
        const auto order = graph->GetExecutionOrder();
        EXPECT_TRUE(std::ranges::any_of(order, [passName](const FString& name)
                                        { return name.ToView() == passName; }))
            << passName << " was culled; this frame does not exercise the affected pass";
        const auto hazards = graph->ValidateCompiledResourceHazards();
        for (const auto& hazard : hazards)
            ADD_FAILURE() << hazard.Message.ToStdString();
    }
} // namespace OloEngine::Tests
