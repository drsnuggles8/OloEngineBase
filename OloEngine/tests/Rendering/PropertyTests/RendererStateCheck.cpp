#include "OloEnginePCH.h"
#include "RendererStateCheck.h"

#include "OloEngine/Renderer/Renderer3D.h"

#include "TestOptions.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace OloEngine::Tests::RendererState
{
    namespace
    {
        // Every entry in a Snapshot is compared byte-for-byte, and `Capture`
        // fills each one with `std::memcpy` from the live object rather than by
        // assignment. That pairing is what makes the comparison sound.
        //
        // Assignment would not be enough: the implicitly-defined copy assignment
        // operator is specified as MEMBERWISE, so it need not carry a struct's
        // padding bytes across, and most of these structs are padded — a leading
        // `bool Enabled` followed by an `f32` leaves three bytes that no member
        // owns. Every mainstream compiler implements a trivial copy as a byte
        // copy and it works out, but relying on that is relying on something the
        // standard does not promise. `memcpy` promises it: both snapshots are
        // verbatim images of the same object, so they differ in a padding byte
        // only if something actually wrote one.
        //
        // Byte comparison rather than field-by-field is also the deliberate
        // choice for WHAT this measures. These structs are mostly `f32` and
        // `glm::vec3`, and CLAUDE.md forbids `==` on both; a semantic comparison
        // would need epsilons, and an epsilon is exactly how a small real leak
        // stops being reported. "Not one bit moved" is the contract a test is
        // being held to here.
        //
        // A type with a `std::string` or a `Ref<T>` in it would compare pointer
        // values and report a spurious leak, so it is rejected outright rather
        // than silently mis-measured. If this fires, the settings struct grew a
        // non-trivial member: give it an `operator==` and compare it explicitly
        // instead of adding it to the memcmp path.
        template<typename T>
        [[nodiscard]] bool BytesEqual(const T& a, const T& b)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "RendererState::Snapshot compares entries with memcmp; a non-trivially "
                          "copyable settings struct must be compared field-wise instead.");
            return std::memcmp(&a, &b, sizeof(T)) == 0;
        }

        // The single place that knows which entries a Snapshot holds. Capture,
        // Describe and Restore all walk this list, so an entry added to one is
        // an entry added to all three — the failure mode that produced this
        // whole issue was a save/restore pair that covered different sets.
#define OLO_RENDERER_STATE_STRUCT_ENTRIES(X)                                                   \
    X(Renderer, "RendererSettings", Renderer3D::GetRendererSettings())                         \
    X(PostProcess, "PostProcessSettings", Renderer3D::GetPostProcessSettings())                \
    X(Snow, "SnowSettings", Renderer3D::GetSnowSettings())                                     \
    X(Fog, "FogSettings", Renderer3D::GetFogSettings())                                        \
    X(Wind, "WindSettings", Renderer3D::GetWindSettings())                                     \
    X(SnowAccumulation, "SnowAccumulationSettings", Renderer3D::GetSnowAccumulationSettings()) \
    X(WaterDisturbance, "WaterDisturbanceSettings", Renderer3D::GetWaterDisturbanceSettings()) \
    X(WaterWakeShape, "WaterWakeSettings", Renderer3D::GetWaterWakeSettings())                 \
    X(WaterFoamAdvection, "WaterFoamSettings", Renderer3D::GetWaterFoamSettings())             \
    X(SnowEjecta, "SnowEjectaSettings", Renderer3D::GetSnowEjectaSettings())                   \
    X(Precipitation, "PrecipitationSettings", Renderer3D::GetPrecipitationSettings())          \
    X(Cloudscape, "CloudscapeRenderState", Renderer3D::GetCloudscapeState())                   \
    X(UnderwaterFog, "UnderwaterFogState", Renderer3D::GetUnderwaterFogState())

        // The scalar toggles, which have distinct getters and setters rather
        // than a single by-reference accessor.
#define OLO_RENDERER_STATE_SCALAR_ENTRIES(X)                                                       \
    X(FrustumCulling, "FrustumCullingEnabled", Renderer3D::IsFrustumCullingEnabled(),              \
      Renderer3D::EnableFrustumCulling)                                                            \
    X(DynamicCulling, "DynamicCullingEnabled", Renderer3D::IsDynamicCullingEnabled(),              \
      Renderer3D::EnableDynamicCulling)                                                            \
    X(DepthAwareClusterCulling, "DepthAwareClusterCullingEnabled",                                 \
      Renderer3D::IsDepthAwareClusterCullingEnabled(), Renderer3D::EnableDepthAwareClusterCulling) \
    X(OcclusionCulling, "OcclusionCullingEnabled", Renderer3D::IsOcclusionCullingEnabled(),        \
      Renderer3D::EnableOcclusionCulling)                                                          \
    X(HZBOcclusionCulling, "HZBOcclusionCullingEnabled",                                           \
      Renderer3D::IsHZBOcclusionCullingEnabled(), Renderer3D::EnableHZBOcclusionCulling)           \
    X(CullingCameraFrozen, "CullingCameraFrozen", Renderer3D::IsCullingCameraFrozen(),             \
      Renderer3D::SetCullingCameraFrozen)                                                          \
    X(CameraRelative, "CameraRelativeEnabled", Renderer3D::IsCameraRelativeEnabled(),              \
      Renderer3D::SetCameraRelativeEnabled)                                                        \
    X(SelectionOutline, "SelectionOutlineEnabled", Renderer3D::IsSelectionOutlineEnabled(),        \
      Renderer3D::SetSelectionOutlineEnabled)                                                      \
    X(ForceDisableCulling, "ForceDisableCulling", Renderer3D::IsForceDisableCulling(),             \
      Renderer3D::SetForceDisableCulling)

        u32 s_LeakCount = 0;

        // test full name -> the state entries it leaked, for the end-of-run
        // summary. A map keeps the summary stable and deduplicated when a test
        // runs more than once (--gtest_repeat).
        std::map<std::string, std::string>& LeakLedger()
        {
            static std::map<std::string, std::string> ledger;
            return ledger;
        }
    } // namespace

    bool Capture(Snapshot& out)
    {
        out = Snapshot{};
        if (!Renderer3D::IsInitialized())
            return false;

        // memcpy, not assignment — see BytesEqual: the comparison is byte-wise,
        // so the capture has to carry padding bytes too or two snapshots of an
        // unchanged struct could differ in bytes no member owns.
#define OLO_CAPTURE_STRUCT(member, label, accessor)                            \
    static_assert(std::is_trivially_copyable_v<decltype(out.member)>,          \
                  "RendererState::Snapshot entries are captured with memcpy"); \
    std::memcpy(&out.member, &(accessor), sizeof(out.member));
        OLO_RENDERER_STATE_STRUCT_ENTRIES(OLO_CAPTURE_STRUCT)
#undef OLO_CAPTURE_STRUCT

#define OLO_CAPTURE_SCALAR(member, label, getter, setter) out.member = (getter);
        OLO_RENDERER_STATE_SCALAR_ENTRIES(OLO_CAPTURE_SCALAR)
#undef OLO_CAPTURE_SCALAR

        out.RenderScale = Renderer3D::GetRenderScale();
        out.Valid = true;
        return true;
    }

    u32 Describe(const Snapshot& before, const Snapshot& after, std::string& outDetail)
    {
        // A test that initialized or shut down the renderer has no before/after
        // pair. That is not a leak, and guessing at one would make the first
        // GPU test in every run permanently guilty.
        if (!before.Valid || !after.Valid)
            return 0;

        std::vector<std::string> changed;

#define OLO_DIFF_STRUCT(member, label, accessor)  \
    if (!BytesEqual(before.member, after.member)) \
        changed.emplace_back(label);
        OLO_RENDERER_STATE_STRUCT_ENTRIES(OLO_DIFF_STRUCT)
#undef OLO_DIFF_STRUCT

#define OLO_DIFF_SCALAR(member, label, getter, setter)                                        \
    if (before.member != after.member)                                                        \
        changed.emplace_back(std::string(label) + " (" + (before.member ? "true" : "false") + \
                             " -> " + (after.member ? "true" : "false") + ")");
        OLO_RENDERER_STATE_SCALAR_ENTRIES(OLO_DIFF_SCALAR)
#undef OLO_DIFF_SCALAR

        // Never `!=` on a float (CLAUDE.md); a render scale that moved by less
        // than this is not something a test set deliberately.
        if (std::abs(before.RenderScale - after.RenderScale) > 1e-6f)
            changed.emplace_back("RenderScale");

        if (changed.empty())
            return 0;

        for (sizet i = 0; i < changed.size(); ++i)
        {
            if (i != 0)
                outDetail += ", ";
            outDetail += changed[i];
        }
        return static_cast<u32>(changed.size());
    }

    void Restore(const Snapshot& snapshot)
    {
        if (!snapshot.Valid || !Renderer3D::IsInitialized())
            return;

        // The by-reference settings accessors return an lvalue, so assignment
        // through them writes the live struct. Cloudscape and underwater fog
        // are const-getter / setter pairs and are handled after the macro.
        Renderer3D::GetRendererSettings() = snapshot.Renderer;
        Renderer3D::GetPostProcessSettings() = snapshot.PostProcess;
        Renderer3D::GetSnowSettings() = snapshot.Snow;
        Renderer3D::GetFogSettings() = snapshot.Fog;
        Renderer3D::GetWindSettings() = snapshot.Wind;
        Renderer3D::GetSnowAccumulationSettings() = snapshot.SnowAccumulation;
        Renderer3D::GetWaterDisturbanceSettings() = snapshot.WaterDisturbance;
        Renderer3D::GetWaterWakeSettings() = snapshot.WaterWakeShape;
        Renderer3D::GetWaterFoamSettings() = snapshot.WaterFoamAdvection;
        Renderer3D::GetSnowEjectaSettings() = snapshot.SnowEjecta;
        Renderer3D::GetPrecipitationSettings() = snapshot.Precipitation;
        Renderer3D::SetCloudscapeState(snapshot.Cloudscape);
        Renderer3D::SetUnderwaterFogState(snapshot.UnderwaterFog);

#define OLO_RESTORE_SCALAR(member, label, getter, setter) setter(snapshot.member);
        OLO_RENDERER_STATE_SCALAR_ENTRIES(OLO_RESTORE_SCALAR)
#undef OLO_RESTORE_SCALAR

        Renderer3D::SetRenderScale(snapshot.RenderScale);

        // Restoring the settings STRUCT is not the same as restoring the
        // renderer. `ApplyRendererSettings` is what pushes `Path` and the AO
        // technique into the render graph's "configured-for" state; without it
        // the structs read correct while the graph stays built for whatever the
        // polluting test switched it to — the same bug, one level down, and
        // invisible to this guard's own comparison. Cheap when nothing moved:
        // the expensive ConfigureRenderGraph is guarded behind an actual
        // path/AO mismatch.
        Renderer3D::ApplyRendererSettings();
    }

    u32 LeakCount()
    {
        return s_LeakCount;
    }

    // =========================================================================
    // GoogleTest listener — restores (and accounts for) renderer configuration
    // around every test.
    // =========================================================================
    namespace
    {
        class RendererStateListener : public ::testing::EmptyTestEventListener
        {
          public:
            void OnTestStart(const ::testing::TestInfo&) override
            {
                Capture(m_Before);
            }

            void OnTestEnd(const ::testing::TestInfo& info) override
            {
                Snapshot after;
                Capture(after);

                std::string detail;
                const u32 changed = Describe(m_Before, after, detail);

                // Restore before anything else can run, whether or not this
                // test is one we would report. A SKIPPED test that got far
                // enough to write a setting leaks exactly as a failing one
                // does, and the next test inherits it either way.
                Restore(m_Before);

                if (changed == 0)
                    return;

                ++s_LeakCount;
                LeakLedger()[std::string(info.test_suite_name()) + "." + info.name()] = detail;

                if (!Options().StrictRendererState)
                    return;

                const ::testing::TestResult* result = info.result();
                if (result != nullptr && result->Skipped())
                    return;

                // Attribute the failure to the polluting test's own source
                // location, not this file, so "jump to failure" lands on the
                // culprit. OnTestEnd fires in reverse listener order, so this
                // is recorded before the result printer reads the result and
                // the test correctly prints [ FAILED ].
                const char* const file = info.file() != nullptr ? info.file() : __FILE__;
                const int line = info.file() != nullptr ? info.line() : __LINE__;
                ADD_FAILURE_AT(file, line)
                    << "Renderer configuration not restored after test; leaked: " << detail
                    << ". This test changed process-global renderer state that every later test in "
                       "this binary shares. The leak was restored here so it cannot corrupt a later "
                       "test (see issue #1074), but fix the source: save and restore what you "
                       "change, or move the change behind the fixture that owns it.";
            }

            void OnTestProgramEnd(const ::testing::UnitTest&) override
            {
                const auto& ledger = LeakLedger();
                if (ledger.empty())
                    return;

                // Printed unconditionally, including on an otherwise green run.
                // A leak that has been repaired is still a bug in the test that
                // caused it, and this is the only place it is visible when
                // --olo-strict-renderer-state is off.
                std::printf("\n[ RENDERER STATE ] %zu test(s) leaked process-global renderer "
                            "configuration; each leak was restored so it could not affect a later "
                            "test. Re-run with --olo-strict-renderer-state to fail them.\n",
                            ledger.size());
                for (const auto& [testName, detail] : ledger)
                    std::printf("[ RENDERER STATE ]   %s: %s\n", testName.c_str(), detail.c_str());
                std::fflush(stdout);
            }

          private:
            Snapshot m_Before;
        };

        bool s_Registered = false;
    } // namespace

    void RegisterListener()
    {
        if (s_Registered)
            return;
        s_Registered = true;
        ::testing::UnitTest::GetInstance()->listeners().Append(new RendererStateListener());
    }
} // namespace OloEngine::Tests::RendererState
