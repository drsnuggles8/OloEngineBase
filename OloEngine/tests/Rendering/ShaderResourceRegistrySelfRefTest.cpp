// OLO_TEST_LAYER: plumbing
// =============================================================================
// ShaderResourceRegistrySelfRefTest.cpp
//
// Issue #841 — 115 OpenGL shader programs survived Renderer::Shutdown() on
// every editor close, while the identical Vulkan path released the same set
// cleanly.
//
// Root cause: OpenGLShader::InitializeResourceRegistry() called
// m_ResourceRegistry.SetShader(shaderRef), and ShaderResourceRegistry::m_Shader
// used to be a Ref<Shader>. ShaderResourceRegistry is a BY-VALUE member of the
// OpenGLShader it describes (OpenGLShader::m_ResourceRegistry), so that stored
// a strong reference from the shader back to itself — a self-referential
// cycle. Every external Ref<Shader> could be dropped (ShaderLibrary::Clear(),
// every s_Data.*Shader.Reset() in Renderer3D::Shutdown(), a pass's own
// Ref<Shader> member going away) and the object's refcount still never
// reached zero, so ~OpenGLShader() — and the OLO_TRACK_DEALLOC / glDeleteProgram
// it performs — never ran. VulkanShader never calls InitializeResourceRegistry,
// which is why only the OpenGL backend showed the leak.
//
// The fix (ShaderResourceRegistry.h) stores a non-owning `const Shader*`
// instead: the registry can never outlive its owning shader (it is not
// separately heap-allocated), so a raw back-pointer is safe by construction.
//
// This test pins the observable contract: once every external Ref<Shader> is
// dropped for a shader whose resource registry has been initialized (the
// self-ref would have been established by then), the shader must actually be
// destroyed.
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"

#include <gtest/gtest.h>

namespace OloEngine::Tests
{
    TEST(ShaderResourceRegistrySelfRefTest, DroppingExternalRefsDestroysShaderAfterRegistryInit)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        WeakRef<Shader> weak;
        {
            ShaderLibrary library;
            Ref<Shader> shader = library.Load("assets/shaders/PBR_MultiLight.glsl");
            ASSERT_TRUE(shader);

            // Force any async link to complete synchronously — this is also
            // the call site (ShaderLibrary::FlushPendingShaders /
            // PollPendingShaders) that invokes InitializeResourceRegistry(),
            // the step that would establish the self-cycle if the bug were
            // still present.
            library.FlushPendingShaders();
            ASSERT_TRUE(shader->IsReady());
            ASSERT_NE(shader->GetResourceRegistry(), nullptr);
            ASSERT_TRUE(shader->GetResourceRegistry()->IsInitialized());

            weak = shader;
            shader.Reset();
            library.Clear();
        }

        EXPECT_FALSE(weak.IsValid())
            << "Shader survived after every external Ref<Shader> was dropped — "
               "ShaderResourceRegistry is holding a self-referential strong ref again "
               "(see docs/agent-rules/lazy-static-release-ownership.md).";
    }
} // namespace OloEngine::Tests
