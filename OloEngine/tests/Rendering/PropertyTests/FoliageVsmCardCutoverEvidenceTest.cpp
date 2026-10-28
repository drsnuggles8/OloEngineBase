#include "OloEnginePCH.h"
#include "FoliageCostTestFixture.h"
#include "OloEngine/Renderer/Framebuffer.h"

// OLO_TEST_LAYER: L8

namespace OloEngine::Tests
{
    class FoliageVsmCardCutoverEvidenceTest : public FoliageCostBaselineTest
    {
    };

    // THE VSM CARD CUT-OVER (#1580): FoliageShadowInvalidation::FirstCardLevel.
    // Clip levels past the one a receiver inside the mesh reach selects cast
    // foliage through cards. Three arms at one pinned clock, every foliage page
    // redrawn (Mode::EveryFrame) so the arms differ only in what was cast:
    //   Meshes  OLO_FOLIAGE_VSM_CARD_LEVEL=16, the pre-cut-over shadow;
    //   Policy  what ships;
    //   Cards   cards in every level -- the POSITIVE CONTROL, which must change
    //           the shadow, or the comparison could not see a card shadow.
    // On the VSM shadow factor (DebugMode 7, what the lit pass receives) and
    // the beauty frame, at three traversal poses on every path. The policy may
    // differ from the meshes only where a receiver beyond the mesh band reads
    // a long shadow from a plant inside it, so its moved pixels are bounded by
    // a small share of the control's. Writes
    // OloEditor/assets/tests/visual/FoliageVsmCardCutover_GL_<Path>_<Pose>_{Meshes,Policy,ShadowFactorDiffx8}.png.
    TEST_F(FoliageVsmCardCutoverEvidenceTest, VsmCardCutoverKeepsTheShadowOfTheMeshBand)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_NO_FATAL_FAILURE(Load(kTraversalScene));
        const auto path = DeriveTraversal();
        ASSERT_TRUE(path.has_value()) << "no meadow-to-woodland path through the committed scene";
        ASSERT_TRUE(SetVsm(true)) << "VSM did not come up";
        namespace FSI = FoliageShadowInvalidation;
        const FSI::Mode previousMode = FSI::GetMode();
        const std::optional<i64> previousLever = Levers::FoliageVsmCardLevel();
        struct Restore
        {
            FSI::Mode Mode;
            std::optional<i64> Lever;
            ~Restore()
            {
                FSI::SetMode(Mode);
                Levers::SetFoliageVsmCardLevel(Lever);
                auto settings = Renderer3D::GetShadowMap().GetSettings();
                settings.VSM.DebugMode = 0;
                Renderer3D::GetShadowMap().SetSettings(settings);
            }
        } restore{ previousMode, previousLever };
        FSI::SetMode(FSI::Mode::EveryFrame);

        const auto setDebug = [](i32 mode)
        {
            auto settings = Renderer3D::GetShadowMap().GetSettings();
            settings.VSM.DebugMode = mode;
            Renderer3D::GetShadowMap().SetSettings(settings);
        };
        const auto read = [this](std::vector<u8>& out)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "no SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentHandle(0), m_Width, m_Height, out);
            ASSERT_EQ(out.size(), static_cast<sizet>(m_Width) * m_Height * 4u);
            const sizet rowBytes = static_cast<sizet>(m_Width) * 4u; // GL readback is bottom-up
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < m_Height / 2u; ++y)
            {
                u8* top = out.data() + static_cast<sizet>(y) * rowBytes;
                u8* bot = out.data() + static_cast<sizet>(m_Height - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
        };
        // Pixels whose luminance moved by more than 8 levels.
        const auto moved = [](const std::vector<u8>& a, const std::vector<u8>& b)
        {
            u32 n = 0;
            for (sizet i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
            {
                const f64 la = 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
                const f64 lb = 0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2];
                n += std::abs(la - lb) > 8.0 ? 1u : 0u;
            }
            return n;
        };
        const auto diffx8 = [](const std::vector<u8>& a, const std::vector<u8>& b)
        {
            std::vector<u8> out(a.size(), 255u);
            for (sizet i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
                for (sizet c = 0; c < 3; ++c)
                    out[i + c] = static_cast<u8>(std::min(255, 8 * std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]))));
            return out;
        };
        const auto writePng = [this](const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = EditorRoot() / "assets" / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string file = (dir / (name + ".png")).string();
            ASSERT_NE(stbi_write_png(file.c_str(), static_cast<int>(m_Width), static_cast<int>(m_Height), 4, pixels.data(),
                                     static_cast<int>(m_Width) * 4),
                      0)
                << file;
        };
        // The arm's foliage casters, summed over the clip levels, cheapest of a
        // few frames. A frame with no valid bracket is no sample (infinity), not
        // a 0 ms one that would win the minimum.
        const auto castersMs = []()
        {
            f64 total = 0.0;
            bool any = false;
            for (const auto& pass : GPUPassTimerPool::GetInstance().GetLastFrameTimings().Passes)
                if (pass.Name == FString("ShadowPass/VsmSun/FoliageCasters") && pass.IsValid())
                {
                    total += pass.Sample.GpuMs;
                    any = true;
                }
            return any ? total : std::numeric_limits<f64>::infinity();
        };
        struct Arm
        {
            const char* Name;
            std::optional<i64> Lever;
            std::vector<u8> Factor;
            std::vector<u8> Beauty;
            f64 CastersMs = 0.0;
        };

        constexpr std::array<std::pair<const char*, RenderingPath>, 3> paths{ { { "Forward", RenderingPath::Forward },
                                                                                { "ForwardPlus", RenderingPath::ForwardPlus },
                                                                                { "Deferred", RenderingPath::Deferred } } };
        for (const auto& [pathName, renderPath] : paths)
        {
            SetPath(renderPath);
            for (const u32 step : { 0u, 4u, 8u })
            {
                const Pose& pose = (*path)[step];
                SCOPED_TRACE(std::string(pathName) + " " + pose.Name);
                std::array<Arm, 4> arms{ { { "Meshes", 16 }, { "Policy", std::nullopt }, { "Cards", 0 }, { "MeshesAgain", 16 } } };
                for (Arm& arm : arms)
                {
                    Levers::SetFoliageVsmCardLevel(arm.Lever);
                    setDebug(0);
                    Tick(pose, 6);
                    f64 best = std::numeric_limits<f64>::infinity();
                    for (u32 frame = 0; frame < 5; ++frame)
                    {
                        Tick(pose, 1);
                        best = std::min(best, castersMs());
                    }
                    arm.CastersMs = best;
                    ASSERT_NO_FATAL_FAILURE(read(arm.Beauty));
                    setDebug(7);
                    Tick(pose, 3);
                    ASSERT_NO_FATAL_FAILURE(read(arm.Factor));
                }
                const Arm& meshes = arms[0];
                const Arm& policy = arms[1];
                const Arm& cards = arms[2];
                const Arm& again = arms[3];
                const u32 noise = moved(meshes.Factor, again.Factor);
                const u32 shipped = moved(meshes.Factor, policy.Factor);
                const u32 control = moved(meshes.Factor, cards.Factor);
                const u32 beauty = moved(meshes.Beauty, policy.Beauty);
                const std::string stem = std::string("FoliageVsmCardCutover_GL_") + pathName + "_" + pose.Name;
                writePng(stem + "_Meshes", meshes.Beauty);
                writePng(stem + "_Policy", policy.Beauty);
                writePng(stem + "_ShadowFactorDiffx8", diffx8(meshes.Factor, policy.Factor));
                writePng(stem + "_ShadowFactorCardsDiffx8", diffx8(meshes.Factor, cards.Factor));
                // Each layer's mesh reach and the first level it casts as cards.
                const auto& vsmSettings = Renderer3D::GetShadowMap().GetVirtualShadowMap().GetSettings();
                FSI::CardCutover settings;
                settings.Clip0HalfExtent = vsmSettings.Clip0HalfExtent;
                settings.ClipSelectionBias = vsmSettings.ClipSelectionBias;
                std::string perLayer;
                for (u32 layer = 0; layer < Plants().GetLayerCount(); ++layer)
                {
                    char entry[96];
                    std::snprintf(entry, sizeof(entry), "%s%s %.0f m->L%u", perLayer.empty() ? "" : ", ",
                                  FoliageOf().m_Layers[static_cast<i32>(layer)].Name.ToStdString().c_str(),
                                  Plants().GetLayerShadowMeshReach(layer), Plants().GetLayerFirstCardLevel(layer, settings));
                    perLayer += entry;
                }
                std::printf("[card-cutover] %-11s %-5s shadow factor moved px: policy %u, cards everywhere %u, repeat %u; "
                            "beauty policy %u; VsmSun/FoliageCasters ms: meshes %.2f, policy %.2f, cards %.2f; cut-over: %s\n",
                            pathName, pose.Name.c_str(), shipped, control, noise, beauty, meshes.CastersMs, policy.CastersMs,
                            cards.CastersMs, perLayer.c_str());
                // The control must stand well clear of the repeat's noise, or a
                // card shadow could go unseen.
                EXPECT_GE(control, std::max<u32>(16u * noise, 200u))
                    << "POSITIVE CONTROL: cards in every level did not change the shadow, so the comparison is blind";
                // The prediction is NO change at the level a receiver selects:
                // every level a receiver this side of a layer's mesh band selects
                // still casts that layer's meshes. What it does not cover is the
                // sampler's fallback walk (vsmShadowFactor tries two coarser
                // levels when a page is not resident): on Forward and Forward+ the
                // densest foreground grass takes it, and two levels coarser can be
                // a card level. Measured at the woodland pose: 44 of 2,073,600 px,
                // by at most 31/255 (accepted with the per-layer rule, #1580).
                // Deferred, which marks those pages, must show no change at all.
                const u32 fallbackAllowance = renderPath == RenderingPath::Deferred ? 0u : 100u;
                EXPECT_LE(shipped, std::max<u32>(4u * noise, 25u) + fallbackAllowance)
                    << "the cut-over moved the shadow beyond the repeat's noise and the fallback walk's share";
            }
        }
    }

} // namespace OloEngine::Tests
