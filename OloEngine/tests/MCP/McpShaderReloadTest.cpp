// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure result-shaping behind olo_shader_reload (issue #316
// Part 4 — the shader inner-loop harness). The status-token mapping and the
// response JSON schema live in a header-only free function with no renderer /
// editor / GPU dependencies, precisely so they can be exercised here without a
// live editor or GL context — the same split McpRenderExplain.h /
// McpPhysicsExplain.h use (the MCP test binary compiles the dispatch core but
// deliberately NOT McpTools.cpp, the editor-backed handler). The live reload is
// verified separately over the MCP attach loop; this pins the status contract
// the agent reads ("ready"/"failed"/...) and the response shape.
//
// Issue #607 additions:
//   * the ShaderRegistry name -> shader lookup that makes PASS-OWNED shaders
//     (VirtualMeshGBuffer, the VirtualCluster*/GTAO/... compute shaders)
//     reloadable by name at all — exercised here against GL-free stub shaders,
//     since the registry deliberately knows nothing about GL;
//   * the FluidIntermediatesPass capture-target names an agent passes to
//     olo_render_capture_target.
#include "MCP/McpShaderReload.h"

#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"
#include "OloEngine/Renderer/ShaderRegistry.h"

#include <string>

namespace
{
    using OloEngine::ComputeShader;
    using OloEngine::Ref;
    using OloEngine::Shader;
    using OloEngine::ShaderCompilationStatus;
    using OloEngine::ShaderRegistry;
    using OloEngine::MCP::ShaderReload::KindToken;
    using OloEngine::MCP::ShaderReload::Result;
    using OloEngine::MCP::ShaderReload::ShaderKind;
    using OloEngine::MCP::ShaderReload::StatusToken;
    using OloEngine::MCP::ShaderReload::ToJson;
    using Json = nlohmann::json;

    // GL-free stand-ins. The registry stores raw pointers and hands back strong
    // Refs via WeakRef::Lock(), so it only needs RefCounted objects with a name —
    // no GL context, no shader compilation.
    class StubShader final : public Shader
    {
      public:
        explicit StubShader(std::string name) : m_Name(std::move(name)) {}

        void Bind() const override {}
        void Unbind() const override {}
        void SetInt(const std::string&, int) const override {}
        void SetIntArray(const std::string&, int*, u32) const override {}
        void SetFloat(const std::string&, f32) const override {}
        void SetFloat2(const std::string&, const glm::vec2&) const override {}
        void SetFloat3(const std::string&, const glm::vec3&) const override {}
        void SetFloat4(const std::string&, const glm::vec4&) const override {}
        void SetMat4(const std::string&, const glm::mat4&) const override {}
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 7u;
        }
        [[nodiscard]] const std::string& GetName() const override
        {
            return m_Name;
        }
        [[nodiscard]] const std::string& GetFilePath() const override
        {
            return m_Name;
        }
        void Reload() override
        {
            ++m_ReloadCount;
        }
        OloEngine::ShaderResourceRegistry* GetResourceRegistry() override
        {
            return nullptr;
        }
        [[nodiscard]] const OloEngine::ShaderResourceRegistry* GetResourceRegistry() const override
        {
            return nullptr;
        }

        [[nodiscard]] int ReloadCount() const
        {
            return m_ReloadCount;
        }

      private:
        std::string m_Name;
        int m_ReloadCount = 0;
    };

    class StubComputeShader final : public ComputeShader
    {
      public:
        explicit StubComputeShader(std::string name) : m_Name(std::move(name)) {}

        void Bind() const override {}
        void Unbind() const override {}
        void SetInt(const std::string&, int) const override {}
        void SetUint(const std::string&, u32) const override {}
        void SetIntArray(const std::string&, int*, u32) const override {}
        void SetFloat(const std::string&, f32) const override {}
        void SetFloat2(const std::string&, const glm::vec2&) const override {}
        void SetFloat3(const std::string&, const glm::vec3&) const override {}
        void SetFloat4(const std::string&, const glm::vec4&) const override {}
        void SetMat4(const std::string&, const glm::mat4&) const override {}
        [[nodiscard]] bool IsValid() const override
        {
            return true;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return 11u;
        }
        [[nodiscard]] const std::string& GetName() const override
        {
            return m_Name;
        }
        [[nodiscard]] const std::string& GetFilePath() const override
        {
            return m_Name;
        }
        void Reload() override
        {
            ++m_ReloadCount;
        }

        [[nodiscard]] int ReloadCount() const
        {
            return m_ReloadCount;
        }

      private:
        std::string m_Name;
        int m_ReloadCount = 0;
    };

    // The real shaders register themselves from their constructors; the stubs are
    // registered by hand so the fixture starts from a known, empty registry.
    class ShaderRegistryTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            ShaderRegistry::Get().Clear();
        }
        void TearDown() override
        {
            ShaderRegistry::Get().Clear();
        }
    };
} // namespace

TEST(McpShaderReload, StatusTokenMapsEveryEnumToLowercase)
{
    EXPECT_STREQ("pending", StatusToken(ShaderCompilationStatus::Pending));
    EXPECT_STREQ("compiling", StatusToken(ShaderCompilationStatus::Compiling));
    EXPECT_STREQ("ready", StatusToken(ShaderCompilationStatus::Ready));
    EXPECT_STREQ("failed", StatusToken(ShaderCompilationStatus::Failed));
}

TEST(McpShaderReload, CleanReloadIsReadyWithEmptyLog)
{
    Result r;
    r.Name = "PBR";
    r.Found = true;
    r.Libraries = { "Renderer3D" };
    r.Status = ShaderCompilationStatus::Ready;
    r.Ok = true;
    r.RendererId = 42;
    r.Log = "";

    const Json j = ToJson(r);
    EXPECT_EQ("PBR", j.at("name").get<std::string>());
    EXPECT_TRUE(j.at("found").get<bool>());
    EXPECT_EQ("ready", j.at("status").get<std::string>());
    EXPECT_TRUE(j.at("ok").get<bool>());
    EXPECT_EQ(42u, j.at("rendererId").get<u32>());
    EXPECT_TRUE(j.at("log").get<std::string>().empty());
    ASSERT_EQ(1u, j.at("libraries").size());
    EXPECT_EQ("Renderer3D", j.at("libraries").at(0).get<std::string>());
}

TEST(McpShaderReload, FailedReloadCarriesStatusAndLog)
{
    Result r;
    r.Name = "Water";
    r.Found = true;
    r.Libraries = { "Renderer3D", "Renderer2D" };
    r.Status = ShaderCompilationStatus::Failed;
    r.Ok = false;
    r.RendererId = 0; // a failed link resets the program id
    r.Log = "ERROR: 0:12: 'vec5' : undeclared identifier";

    const Json j = ToJson(r);
    EXPECT_EQ("failed", j.at("status").get<std::string>());
    EXPECT_FALSE(j.at("ok").get<bool>());
    EXPECT_EQ(0u, j.at("rendererId").get<u32>());
    EXPECT_NE(std::string::npos, j.at("log").get<std::string>().find("undeclared identifier"));
    EXPECT_EQ(2u, j.at("libraries").size());
}

TEST(McpShaderReload, NotFoundReportsFoundFalseAndNoLibraries)
{
    Result r;
    r.Name = "DoesNotExist";
    r.Found = false; // handler returns an error before shaping when truly absent,
                     // but the schema must still degrade cleanly if asked to shape.

    const Json j = ToJson(r);
    EXPECT_FALSE(j.at("found").get<bool>());
    EXPECT_FALSE(j.at("ok").get<bool>());
    EXPECT_TRUE(j.at("libraries").empty());
    EXPECT_TRUE(j.at("log").get<std::string>().empty());
}

// --- issue #607: pass-owned shaders are reloadable -------------------------

TEST(McpShaderReload, KindTokenDistinguishesGraphicsFromCompute)
{
    EXPECT_STREQ("graphics", KindToken(ShaderKind::Graphics));
    EXPECT_STREQ("compute", KindToken(ShaderKind::Compute));
}

TEST(McpShaderReload, DefaultKindIsGraphicsAndIsReportedInJson)
{
    Result r;
    r.Name = "VirtualMeshGBuffer";
    EXPECT_EQ("graphics", ToJson(r).at("kind").get<std::string>());
}

// A reloaded compute shader (e.g. VirtualClusterCull.comp) reports kind=compute
// and the PassOwned owner label — the two fields that tell an agent it just
// hot-reloaded something the old tool refused outright.
TEST(McpShaderReload, PassOwnedComputeReloadIsLabelledAndOk)
{
    Result r;
    r.Name = "VirtualClusterCull";
    r.Found = true;
    r.Kind = ShaderKind::Compute;
    r.Libraries = { OloEngine::MCP::ShaderReload::kPassOwnedLabel };
    r.Status = ShaderCompilationStatus::Ready;
    r.Ok = true;
    r.RendererId = 9;

    const Json j = ToJson(r);
    EXPECT_EQ("compute", j.at("kind").get<std::string>());
    EXPECT_EQ("ready", j.at("status").get<std::string>());
    ASSERT_EQ(1u, j.at("libraries").size());
    EXPECT_EQ("PassOwned", j.at("libraries").at(0).get<std::string>());
}

TEST_F(ShaderRegistryTest, StartsEmpty)
{
    EXPECT_TRUE(ShaderRegistry::Get().GetAllNames().empty());
    EXPECT_FALSE(ShaderRegistry::Get().Contains("VirtualMeshGBuffer"));
}

// The whole point of the registry: a shader that lives in NO ShaderLibrary — a
// render-pass member — still resolves by name, and the resolved Ref can be
// Reload()ed. This is what olo_shader_reload now falls back to.
TEST_F(ShaderRegistryTest, ResolvesAndReloadsAPassOwnedGraphicsShader)
{
    Ref<StubShader> passOwned = Ref<StubShader>::Create("VirtualMeshGBuffer");
    ShaderRegistry::Get().RegisterShader(passOwned->GetName(), passOwned.get());

    EXPECT_TRUE(ShaderRegistry::Get().Contains("VirtualMeshGBuffer"));

    auto found = ShaderRegistry::Get().FindShaders("VirtualMeshGBuffer");
    ASSERT_EQ(1u, found.size());
    found.front()->Reload();
    EXPECT_EQ(1, passOwned->ReloadCount());

    EXPECT_TRUE(ShaderRegistry::Get().FindShaders("NotAShader").empty());
}

TEST_F(ShaderRegistryTest, ResolvesAndReloadsAPassOwnedComputeShader)
{
    Ref<StubComputeShader> compute = Ref<StubComputeShader>::Create("VirtualClusterCull");
    ShaderRegistry::Get().RegisterComputeShader(compute->GetName(), compute.get());

    EXPECT_TRUE(ShaderRegistry::Get().Contains("VirtualClusterCull"));
    // Compute shaders live in their own map — a graphics lookup must not find one
    // (the handler tries graphics first and only then compute).
    EXPECT_TRUE(ShaderRegistry::Get().FindShaders("VirtualClusterCull").empty());

    auto found = ShaderRegistry::Get().FindComputeShaders("VirtualClusterCull");
    ASSERT_EQ(1u, found.size());
    found.front()->Reload();
    EXPECT_EQ(1, compute->ReloadCount());
}

// Two passes can each create the same .glsl (and a library copy may exist too),
// so one name may map to several live shaders — a reload must fan out to all of
// them, or one copy silently keeps running the old code.
TEST_F(ShaderRegistryTest, ReloadFansOutToEveryShaderSharingAName)
{
    Ref<StubShader> first = Ref<StubShader>::Create("SharedShader");
    Ref<StubShader> second = Ref<StubShader>::Create("SharedShader");
    ShaderRegistry::Get().RegisterShader("SharedShader", first.get());
    ShaderRegistry::Get().RegisterShader("SharedShader", second.get());
    // Re-registering the same instance must not duplicate it.
    ShaderRegistry::Get().RegisterShader("SharedShader", first.get());

    auto found = ShaderRegistry::Get().FindShaders("SharedShader");
    ASSERT_EQ(2u, found.size());
    for (auto& shader : found)
        shader->Reload();
    EXPECT_EQ(1, first->ReloadCount());
    EXPECT_EQ(1, second->ReloadCount());
}

// Unregistration is what keeps the raw-pointer map safe: the entry must be gone
// before the shader's memory can be recycled by another allocation, or a later
// lookup could hand out a Ref to something that is not a shader.
TEST_F(ShaderRegistryTest, UnregisterRemovesTheNameEntirely)
{
    Ref<StubShader> shader = Ref<StubShader>::Create("Doomed");
    ShaderRegistry::Get().RegisterShader("Doomed", shader.get());
    ASSERT_TRUE(ShaderRegistry::Get().Contains("Doomed"));

    ShaderRegistry::Get().UnregisterShader(shader.get());

    EXPECT_FALSE(ShaderRegistry::Get().Contains("Doomed"));
    EXPECT_TRUE(ShaderRegistry::Get().FindShaders("Doomed").empty());
    EXPECT_TRUE(ShaderRegistry::Get().GetAllNames().empty());
}

TEST_F(ShaderRegistryTest, GetAllNamesIsSortedDeduplicatedAndSpansBothKinds)
{
    Ref<StubShader> gfx = Ref<StubShader>::Create("VirtualMeshGBuffer");
    Ref<StubShader> gfxDuplicate = Ref<StubShader>::Create("VirtualMeshGBuffer");
    Ref<StubComputeShader> compute = Ref<StubComputeShader>::Create("FluidSmooth");
    ShaderRegistry::Get().RegisterShader("VirtualMeshGBuffer", gfx.get());
    ShaderRegistry::Get().RegisterShader("VirtualMeshGBuffer", gfxDuplicate.get());
    ShaderRegistry::Get().RegisterComputeShader("FluidSmooth", compute.get());

    const std::vector<std::string> names = ShaderRegistry::Get().GetAllNames();
    ASSERT_EQ(2u, names.size());
    EXPECT_EQ("FluidSmooth", names[0]);
    EXPECT_EQ("VirtualMeshGBuffer", names[1]);
}

TEST_F(ShaderRegistryTest, IgnoresEmptyNamesAndNullShaders)
{
    Ref<StubShader> shader = Ref<StubShader>::Create("Ignored");
    ShaderRegistry::Get().RegisterShader("", shader.get());
    ShaderRegistry::Get().RegisterShader("Nulled", nullptr);
    ShaderRegistry::Get().RegisterComputeShader("", nullptr);

    EXPECT_TRUE(ShaderRegistry::Get().GetAllNames().empty());
    // Unregistering something that was never registered must be a no-op, not a crash.
    ShaderRegistry::Get().UnregisterShader(shader.get());
    ShaderRegistry::Get().UnregisterComputeShader(nullptr);
}

// --- issue #607 / #630: the fluid intermediates are capturable --------------

// FluidIntermediatesPass owns its render targets as raw GL textures, so they only
// become visible to olo_render_list_targets / olo_render_capture_target because
// Setup ImportTexture()s them under these names. They are the strings an agent
// types, so pin them: a silent rename breaks every saved bisection recipe.
TEST(McpFluidCaptureTargets, TargetNamesAreStableAndDistinct)
{
    EXPECT_STREQ("FluidSmoothedDepth", OloEngine::FluidIntermediatesPass::kSmoothedDepthTargetName);
    EXPECT_STREQ("FluidThickness", OloEngine::FluidIntermediatesPass::kThicknessTargetName);
    EXPECT_STRNE(OloEngine::FluidIntermediatesPass::kSmoothedDepthTargetName,
                 OloEngine::FluidIntermediatesPass::kThicknessTargetName);
}
