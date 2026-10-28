#pragma once

#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RendererTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace OloEngine::Tests
{
    // Call only after the test's GPU gate. Keep startup lazy, but let the
    // nightly report charge it to the process instead of the first GPU case.
    inline void EnsureProcessRendererInitialized()
    {
        if (Renderer3D::IsInitialized())
            return;

        const auto start = std::chrono::steady_clock::now();
        Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
        const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
        ::testing::Test::RecordProperty("olo_process_renderer_init_s", std::to_string(seconds));
    }
} // namespace OloEngine::Tests
