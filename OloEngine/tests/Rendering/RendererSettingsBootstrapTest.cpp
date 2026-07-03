#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/LightCulling/TiledForwardPlus.h"

// OLO_TEST_LAYER: unit

// =============================================================================
// RendererSettingsBootstrapTest — unit (headless, no GL context required).
//
// Pins the config->live-state sync that issue #534 fixed: Renderer3D's live
// depth-prepass / Forward+ mode are derived from RendererSettings, but that
// derivation only fires inside Renderer3D::ApplyRendererSettings(). Nothing
// called that at editor startup or scene-load, so the editor booted with
// DepthPrepassEnabled=false and Forward+ never auto-switching even though the
// config default asks for both — a measured ~2.8x GPU regression on light-heavy
// scenes until a human toggled a settings-panel control.
//
// These tests are the CI-safe half of the fix (the editor call sites are the
// other half, verified live over MCP): they prove ApplyRendererSettings() makes
// the LIVE renderer state match the settings-DERIVED value, so the boot/scene-load
// calls added to EditorLayer actually apply the defaults. ApplyRendererSettings
// touches no GL when the renderer is uninitialized (the graph rebuild is guarded
// on a null RGraph, the ForwardPlus setters no-op until Initialize()), so the
// contract is exercised without a live context.
//
// Each test mutates the global RendererSettings; the fixture snapshots and
// restores them (and re-derives the live flags) so ordering can't leak state
// into the GL-context renderer tests that share this process.
// =============================================================================

namespace
{
    using OloEngine::ForwardPlusMode;
    using OloEngine::Renderer3D;
    using OloEngine::RendererSettings;
    using OloEngine::RenderingPath;

    class RendererSettingsBootstrapTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Saved = Renderer3D::GetRendererSettings();
        }

        void TearDown() override
        {
            // Restore the config AND re-derive the live flags from it so a later
            // test never observes this test's mutated depth-prepass / Forward+ mode.
            Renderer3D::GetRendererSettings() = m_Saved;
            Renderer3D::ApplyRendererSettings();
        }

        RendererSettings m_Saved{};
    };

    // The shipped default (Forward path + Forward+ auto-switch) derives depth-prepass
    // ON — the config genuinely wants the prepass, so "live prepass == false at boot"
    // is a real divergence, not the intended default.
    TEST_F(RendererSettingsBootstrapTest, DefaultConfigDerivesDepthPrepassOn)
    {
        Renderer3D::GetRendererSettings() = RendererSettings{}; // pristine shipped defaults
        EXPECT_TRUE(Renderer3D::ComputeSettingsDerivedDepthPrepass());
    }

    // The #534 regression, reproduced at the Renderer3D seam and then fixed by the
    // apply call: force the live prepass flag to the wrong value the editor booted
    // with, confirm it diverges from the config-derived value, then ApplyRendererSettings
    // must reconcile them. This is exactly what EditorLayer now invokes at startup /
    // scene-load.
    TEST_F(RendererSettingsBootstrapTest, ApplyReconcilesLiveDepthPrepassWithConfig)
    {
        Renderer3D::GetRendererSettings() = RendererSettings{}; // Forward + auto-switch -> derives true

        // Simulate the un-applied boot state: live flag stuck at its zero default.
        Renderer3D::EnableDepthPrepass(false);
        ASSERT_TRUE(Renderer3D::ComputeSettingsDerivedDepthPrepass());
        ASSERT_FALSE(Renderer3D::IsDepthPrepassEnabled()) << "precondition: live state diverges from config before apply";

        Renderer3D::ApplyRendererSettings();

        EXPECT_EQ(Renderer3D::IsDepthPrepassEnabled(), Renderer3D::ComputeSettingsDerivedDepthPrepass());
        EXPECT_TRUE(Renderer3D::IsDepthPrepassEnabled());
    }

    // The ForwardPlus path forces the tile-culling mode to Always and requires the
    // depth prepass — apply must push both into the live renderer.
    TEST_F(RendererSettingsBootstrapTest, ApplyForwardPlusPathEnablesAlwaysModeAndPrepass)
    {
        RendererSettings settings{};
        settings.Path = RenderingPath::ForwardPlus;
        Renderer3D::GetRendererSettings() = settings;

        Renderer3D::GetForwardPlus().SetMode(ForwardPlusMode::Never); // wrong value to overwrite
        Renderer3D::EnableDepthPrepass(false);

        Renderer3D::ApplyRendererSettings();

        EXPECT_EQ(Renderer3D::GetForwardPlus().GetMode(), ForwardPlusMode::Always);
        EXPECT_TRUE(Renderer3D::IsDepthPrepassEnabled());
    }

    // With auto-switch OFF on the plain Forward path, the derivation drops the prepass
    // — the reconciliation must be able to turn the live flag back off too, not just on.
    TEST_F(RendererSettingsBootstrapTest, ApplyForwardNoAutoSwitchDisablesPrepassAndAutoMode)
    {
        RendererSettings settings{};
        settings.Path = RenderingPath::Forward;
        settings.ForwardPlusAutoSwitch = false;
        settings.DepthPrepassEnabled = false;
        Renderer3D::GetRendererSettings() = settings;

        Renderer3D::EnableDepthPrepass(true); // wrong value to overwrite

        Renderer3D::ApplyRendererSettings();

        EXPECT_FALSE(Renderer3D::ComputeSettingsDerivedDepthPrepass());
        EXPECT_FALSE(Renderer3D::IsDepthPrepassEnabled());
        EXPECT_EQ(Renderer3D::GetForwardPlus().GetMode(), ForwardPlusMode::Never);
    }

    // Culling toggles round-trip through apply too — a scene/config that turns frustum
    // culling off must be honoured at boot, not left at the live default.
    TEST_F(RendererSettingsBootstrapTest, ApplySyncsFrustumCullingToggle)
    {
        RendererSettings settings{};
        settings.FrustumCullingEnabled = false;
        Renderer3D::GetRendererSettings() = settings;

        Renderer3D::EnableFrustumCulling(true); // wrong value to overwrite
        Renderer3D::ApplyRendererSettings();

        EXPECT_FALSE(Renderer3D::IsFrustumCullingEnabled());
    }
} // namespace
