#include "OloEnginePCH.h"

// =============================================================================
// LuaDrivesTerrainRegenerationTest — Functional Test.
//
// Cross-subsystem seam under test (issue #293):
//   Scene tick × LuaScriptEngine::OnUpdateEntity × LuaScriptGlue's
//   `TerrainComponent` usertype (scalar procedural-gen params + the
//   regenerate() trigger) × TerrainComponent::Regenerate() × the
//   TerrainGenerator height-field math the Scene rebuild path consumes.
//
//   Gameplay scripts must be able to drive procedural world generation at
//   runtime — pick a seed per run, regenerate on a level transition, scale
//   shaping from game state — instead of terrain being purely editor-authored.
//
// What this asserts (all headless — the actual GPU terrain rebuild runs only
// from Scene::RenderScene3D → ProcessScene3DSharedLogic, which Functional
// tests deliberately don't drive, so we verify the CPU-observable contract):
//   1. A Lua script, fired through a real Scene::OnUpdateRuntime tick, reads
//      and writes the procedural params (seed + a second param) through the
//      usertype — they round-trip onto the component the engine sees.
//   2. The script's terrain:regenerate() call drops the cached runtime data
//      and arms the rebuild flags, so the next render tick rebuilds the height
//      field from the NEW params instead of reusing the stale TerrainData. (A
//      naive flag-only trigger would silently keep the old field — the Scene
//      rebuild only regenerates when m_TerrainData is null.)
//   3. The script-driven seed change deterministically changes the generated
//      height field: GenerateHeightField(old params) != GenerateHeightField(new
//      params), and regenerating with identical params is bit-stable. This is
//      the pure-CPU half of "the heightmap changes deterministically per seed"
//      that the Scene's GenerateHeightmap → GenerateHeightField would produce.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGenerator.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    std::filesystem::path WriteScript(const std::string& contents, const char* nameStem)
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string fileName = std::string("olo_functional_") + nameStem + "_" + (info ? info->name() : "unknown") + ".lua";
        const auto path = std::filesystem::temp_directory_path() / fileName;
        // Validate the open and the write — a silent I/O failure here would
        // surface much later as a "Lua failed to load script" inside the engine.
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            throw std::runtime_error("WriteScript: failed to open temp file for writing: " + path.string());
        out << contents;
        out.flush();
        if (!out.good())
            throw std::runtime_error("WriteScript: write failed for: " + path.string());
        return path;
    }

    // Mirror Scene::ProcessScene3DSharedLogic's procedural rebuild: build the
    // generator params straight off the component fields the script mutated, so
    // the height field we compare is exactly what the GPU rebuild would consume.
    TerrainGenerator::HeightParams ParamsFromComponent(const TerrainComponent& t)
    {
        TerrainGenerator::HeightParams params;
        params.Resolution = t.m_ProceduralResolution;
        params.Seed = t.m_ProceduralSeed;
        params.Octaves = t.m_ProceduralOctaves;
        params.Frequency = t.m_ProceduralFrequency;
        params.Lacunarity = t.m_ProceduralLacunarity;
        params.Persistence = t.m_ProceduralPersistence;
        params.Shaping = t.m_HeightShaping;
        return params;
    }
} // namespace

class LuaDrivesTerrainRegenerationTest : public FunctionalTest
{
  protected:
    // Small explicit UUID → exact sol2 u64 ↔ Lua double round-trip.
    static constexpr u64 kTerrainUUID = 7777;
    static constexpr i32 kInitialSeed = 42;
    static constexpr i32 kNewSeed = 1337;
    static constexpr u32 kNewOctaves = 7;

    void BuildScene() override
    {
        m_Entity = GetScene().CreateEntityWithUUID(UUID{ kTerrainUUID }, "World");
        auto& terrain = m_Entity.AddComponent<TerrainComponent>();
        terrain.m_ProceduralEnabled = true;
        terrain.m_ProceduralSeed = kInitialSeed;
        terrain.m_ProceduralResolution = 64; // keep the CPU height-field cheap
        terrain.m_ProceduralOctaves = 4;

        EnableLua();

        // On the first OnUpdate: read the seed, switch to a new seed + octaves,
        // then trigger regeneration. Idempotent thereafter so the post-tick
        // state is stable to assert against.
        const std::string scriptSrc = R"(
local script = {}
local applied = false
function script.OnUpdate(entityID, ts)
    if applied then return end
    local id = math.tointeger(entityID) or 0
    local terrain = entity_utils.get_component(id, "TerrainComponent")
    if terrain ~= nil then
        terrain.seed = 1337
        terrain.octaves = 7
        terrain:regenerate()
        applied = true
    end
end
return script
)";
        m_ScriptPath = WriteScript(scriptSrc, "terrain_regen");
        RegisterLuaScript(m_Entity, m_ScriptPath);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_ScriptPath, ec);
    }

    Entity m_Entity;
    std::filesystem::path m_ScriptPath;
};

TEST_F(LuaDrivesTerrainRegenerationTest, LuaSetsSeedAndRegeneratesChangingTheHeightField)
{
    auto& terrain = m_Entity.GetComponent<TerrainComponent>();

    // Baseline: the editor-authored seed, and the height field it produces.
    ASSERT_EQ(terrain.m_ProceduralSeed, kInitialSeed);
    std::vector<f32> fieldBefore;
    TerrainGenerator::GenerateHeightField(fieldBefore, ParamsFromComponent(terrain));
    ASSERT_FALSE(fieldBefore.empty());

    // Prime the runtime cache the way a first render tick would: a non-null
    // TerrainData that a naive flag-only "regenerate" would wrongly reuse, with
    // the rebuild flags cleared (as the rebuild leaves them).
    terrain.m_TerrainData = Ref<TerrainData>::Create();
    terrain.m_NeedsRebuild = false;
    terrain.m_AutoSplatNeedsRebuild = false;

    // One real Scene::OnUpdateRuntime tick: the Lua OnUpdate fires, sets the
    // seed + octaves through the usertype, and calls regenerate(). A second
    // frame is harmless (the script is idempotent after the first).
    RunFrames(2);

    // (1) Params round-tripped onto the component the engine sees.
    EXPECT_EQ(terrain.m_ProceduralSeed, kNewSeed)
        << "Lua's `terrain.seed = …` didn't reach the component — the usertype "
           "setter wrote to a copy, or get_component returned one.";
    EXPECT_EQ(terrain.m_ProceduralOctaves, kNewOctaves)
        << "second gen param (octaves) didn't round-trip through the usertype.";

    // (2) regenerate() armed a real rebuild: cached data dropped, flags set, so
    // the next render tick rebuilds the height field from the NEW params instead
    // of silently reusing the stale TerrainData.
    EXPECT_EQ(terrain.m_TerrainData, nullptr)
        << "regenerate() left the cached TerrainData in place — the Scene rebuild "
           "path only regenerates the height field when m_TerrainData is null, so "
           "the new seed would never take effect.";
    EXPECT_TRUE(terrain.m_NeedsRebuild);
    EXPECT_TRUE(terrain.m_AutoSplatNeedsRebuild);

    // (3) The script-driven param change deterministically changes the height
    // field, and regenerating with identical params is bit-stable. This is the
    // CPU half of the Scene's GPU rebuild (GenerateHeightmap → GenerateHeightField).
    std::vector<f32> fieldAfter;
    TerrainGenerator::GenerateHeightField(fieldAfter, ParamsFromComponent(terrain));
    ASSERT_EQ(fieldAfter.size(), fieldBefore.size());
    EXPECT_NE(fieldAfter, fieldBefore)
        << "the script-set seed/octaves produced an identical height field — the "
           "params aren't actually feeding generation.";

    std::vector<f32> fieldAfterRepeat;
    TerrainGenerator::GenerateHeightField(fieldAfterRepeat, ParamsFromComponent(terrain));
    EXPECT_EQ(fieldAfter, fieldAfterRepeat)
        << "same params produced different fields — generation isn't deterministic.";
}
